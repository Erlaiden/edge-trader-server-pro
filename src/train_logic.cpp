#include "json.hpp"
#include <armadillo>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <ctime>
#include <cerrno>
#include <algorithm>

// Декларация ядра обучения (уже реализовано в ppo_pro.cpp)
namespace etai {
  nlohmann::json trainPPO_pro(const arma::mat& raw15,
                              const arma::mat* raw60,
                              const arma::mat* raw240,
                              const arma::mat* raw1440,
                              int episodes, double tp, double sl, int ma_len);
}

using json = nlohmann::json;
using arma::mat;

// --- Утилита: безопасный clamp ---
static inline double clamp(double v, double lo, double hi){
  if(v < lo) return lo;
  if(v > hi) return hi;
  return v;
}

// --- Утилита: атомарная запись файла (tmp -> rename) ---
static bool atomic_write_file(const std::string& path, const std::string& data, std::string& err){
  std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary);
    if(!f.good()){ err = "open_tmp_fail"; return false; }
    f.write(data.data(), (std::streamsize)data.size());
    if(!f.good()){ err = "write_tmp_fail"; return false; }
    f.flush();
    if(!f.good()){ err = "flush_tmp_fail"; return false; }
  }
  if(std::rename(tmp.c_str(), path.c_str()) != 0){
    err = std::string("rename_fail: ") + std::strerror(errno);
    std::remove(tmp.c_str());
    return false;
  }
  return true;
}

// --- CSV loader: ожидаем cache/SYMBOL_INTERVAL.csv с колонками: ts,open,high,low,close,volume,[turnover?]
static bool load_cached_ohlcv(const std::string& symbol, const std::string& interval,
                              arma::mat& out, std::string& err)
{
  std::string path = "cache/" + symbol + "_" + interval + ".csv";
  std::ifstream f(path);
  if(!f.is_open()){ err = "csv_open_fail:" + path; return false; }

  std::vector<double> row; row.reserve(6);
  std::vector<double> data; data.reserve(7 * 4096);
  std::string line;
  bool header_skipped = false;

  while(std::getline(f, line)){
    if(line.empty()) continue;
    // Пропускаем хедер, если есть
    if(!header_skipped){
      // очень грубо: если первая строка содержит нецифры — считаем заголовком
      bool looks_header = line.find("ts")!=std::string::npos || line.find("open")!=std::string::npos;
      if(looks_header){ header_skipped = true; continue; }
      header_skipped = true;
    }
    std::stringstream ss(line);
    std::string cell;
    row.clear();
    while(std::getline(ss, cell, ',')){
      try{
        // пустые/NaN -> 0
        if(cell.empty()){ row.push_back(0.0); continue; }
        // уберём пробелы
        size_t a=0,b=cell.size();
        while(a<b && std::isspace((unsigned char)cell[a])) ++a;
        while(b>a && std::isspace((unsigned char)cell[b-1])) --b;
        std::string v = cell.substr(a, b-a);
        // ts может быть int → double
        double x = std::stod(v);
        if(!std::isfinite(x)) x = 0.0;
        row.push_back(x);
      }catch(...){
        row.push_back(0.0);
      }
    }
    if(row.size() < 6) continue;       // нужно хотя бы ts, o, h, l, c, v
    // Берём первые 6 столбцов
    for(size_t k=0;k<6;++k) data.push_back(row[k]);
  }

  if(data.size() < 6*100){ err = "csv_too_small"; return false; }

  // Преобразуем в матрицу: N x 6, порядок по строкам
  const size_t N = data.size() / 6;
  out = arma::mat(N, 6);
  for(size_t i=0;i<N;++i){
    for(size_t j=0;j<6;++j){
      out(i,j) = data[i*6 + j];
    }
  }
  return true;
}

// === ПУБЛИЧНАЯ ФУНКЦИЯ (вызывается из роутера /api/train) ===
json run_train_pro_and_save(const std::string& symbol,
                            const std::string& interval,
                            int episodes,
                            double tp,
                            double sl,
                            int ma_len)
{
  json out = json::object();
  try{
    // 1) Грузим 15m данные
    arma::mat raw15;
    std::string err;
    if(!load_cached_ohlcv(symbol, interval, raw15, err)){
      out["ok"] = false;
      out["error"] = "load_data_fail";
      out["error_detail"] = err;
      return out;
    }

    // 2) Тренируем (доп. таймфреймы опциональны — передаём nullptr)
    nlohmann::json M = etai::trainPPO_pro(raw15, nullptr, nullptr, nullptr, episodes, tp, sl, ma_len);

    if(!M.is_object() || !M.value("ok", false)){
      out["ok"] = false;
      out["error"] = "train_failed";
      out["model_raw"] = M;
      return out;
    }

    // 3) Собираем и нормализуем финальную модель перед сохранением
    const std::string schema = M.value("schema","ppo_pro_v1");
    const std::string mode   = M.value("mode","pro");
    double best_thr = M.value("best_thr", 0.0006);
    // разумный коридор для 15m BTC; если другие символы — всё равно безопасно
    best_thr = clamp(best_thr, 1e-4, 1e-2);

    // метрики
    json metrics = json::object();
    if(M.contains("metrics") && M["metrics"].is_object()){
      metrics = M["metrics"];
    }
    metrics["best_thr"] = best_thr; // ensure clamped value отражается в метриках

    // policy
    json policy = json::object();
    if(M.contains("policy")) policy = M["policy"];
    // минимальная валидация policy
    int feat_dim = policy.value("feat_dim", 0);
    if(feat_dim <= 0){
      // вытянем из W, если есть
      if(policy.contains("W") && policy["W"].is_array())
        feat_dim = (int)policy["W"].size();
      policy["feat_dim"] = feat_dim;
    }
    policy["feat_version"] = policy.value("feat_version", 2);

    // финальный объект модели
    json model = {
      {"ok", true},
      {"version", 3},
      {"schema", schema},
      {"mode", mode},
      {"symbol", symbol},
      {"interval", interval},
      {"tp", tp},
      {"sl", sl},
      {"ma_len", ma_len},
      {"best_thr", best_thr},
      {"policy_source", M.value("policy_source","learn")},
      {"policy", policy},
      {"metrics", metrics},
      {"build_ts", (long long) (std::time(nullptr) * 1000LL)}
    };

    // 4) Пишем файл модели
    std::string dir = "cache/models";
    std::string path = dir + "/" + symbol + "_" + interval + "_ppo_pro.json";

    // обеспечим наличие каталога (минимально, через std::system — без <filesystem> зависимостей)
    std::string mk = "mkdir -p " + dir;
    std::system(mk.c_str());

    std::string json_text = model.dump(2);
    std::string werr;
    if(!atomic_write_file(path, json_text, werr)){
      out["ok"] = false;
      out["error"] = "save_fail";
      out["error_detail"] = werr;
      return out;
    }

    // 5) Ответ
    out["ok"] = true;
    out["model_path"] = path;
    out["metrics"] = metrics;
    return out;
  }
  catch(const std::exception& e){
    out["ok"] = false;
    out["error"] = "train_logic_exception";
    out["error_detail"] = e.what();
    return out;
  }
  catch(...){
    out["ok"] = false;
    out["error"] = "train_logic_unknown_exception";
    return out;
  }
}
