#include "utils_data.h"
#include "features.h"            // билдер фич
#include <armadillo>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cctype>

using json = nlohmann::json;
namespace fs = std::filesystem;
using namespace arma;

// Явная декларация, чтобы не зависеть от содержимого features.h
namespace etai {
    arma::Mat<double> build_feature_matrix(const arma::Mat<double>& raw);
}

namespace etai {

// --- helpers ---
static inline std::string upper(std::string s){
  for (auto& c : s) c = (char)std::toupper((unsigned char)c);
  return s;
}
static inline std::string norm_interval(std::string s){
  s.erase(std::remove_if(s.begin(), s.end(),
         [](unsigned char c){ return std::isspace(c) || c=='\"'; }), s.end());
  return s;
}
static inline std::string base_csv_path(const std::string& symbol, const std::string& interval){
  // приоритет: cache/clean (6 колонок) → cache/
  std::string fn = upper(symbol) + "_" + norm_interval(interval) + ".csv";
  std::string p1 = "cache/clean/" + fn;
  std::string p2 = "cache/"       + fn;
  if (fs::exists(p1)) return p1;
  return p2;
}
static inline bool read_csv_first6(const std::string& path, arma::mat& out){
  std::ifstream f(path);
  if(!f.is_open()) return false;
  std::vector<std::array<double,6>> rows;
  std::string line;
  rows.reserve(4096);
  while(std::getline(f,line)){
    if(line.empty()) continue;
    std::stringstream ss(line);
    std::string cell; std::vector<double> vals; vals.reserve(8);
    while(std::getline(ss,cell,',')){
      try { vals.push_back(std::stod(cell)); } catch(...) { vals.push_back(0.0); }
    }
    if(vals.size()>=6){
      std::array<double,6> r{ vals[0], vals[1], vals[2], vals[3], vals[4], vals[5] };
      rows.push_back(r);
    }
  }
  if(rows.empty()) return false;
  out.set_size(rows.size(),6);
  for(size_t i=0;i<rows.size();++i){
    for(int j=0;j<6;++j) out(i,j)=rows[i][j];
  }
  return true;
}

// --- API ---

bool load_raw_ohlcv(const std::string& symbol,
                    const std::string& interval,
                    arma::mat& out)
{
  const std::string path = base_csv_path(symbol, interval);
  if(!fs::exists(path)){
    std::cerr << "[FAIL] raw not found: " << path << std::endl;
    return false;
  }
  if(!read_csv_first6(path, out)){
    std::cerr << "[FAIL] cannot read csv: " << path << std::endl;
    return false;
  }
  if(out.n_cols != 6){
    std::cerr << "[FAIL] raw_cols=" << out.n_cols << " need 6" << std::endl;
    return false;
  }
  if(out.n_rows < 300){
    std::cerr << "[WARN] rows=" << out.n_rows << " (<300) for " << path << std::endl;
    return false;
  }
  return true;
}

bool load_cached_xy(const std::string& symbol,
                    const std::string& interval,
                    arma::mat& X,
                    arma::mat& y)
{
  const std::string base = upper(symbol) + "_" + norm_interval(interval);
  const std::string xfile = "cache/xy/" + base + "_X.csv";
  const std::string yfile = "cache/xy/" + base + "_y.csv";

  // 1) Пытаемся загрузить готовый кэш
  if(fs::exists(xfile) && fs::exists(yfile)){
    try {
      X.load(xfile, arma::csv_ascii);
      y.load(yfile, arma::csv_ascii);
      if(X.n_rows>0 && X.n_cols>0) return true;
    } catch (...) {
      // упадём к построению
    }
  }

  // 2) Строим X из raw через единый билдер
  arma::mat raw;
  if(!load_raw_ohlcv(symbol, interval, raw)){
    std::cerr << "[WARN] load_cached_xy: cannot load raw for "
              << symbol << "_" << interval << std::endl;
    return false;
  }

  arma::Mat<double> F = etai::build_feature_matrix(raw);
  X = F;

  // y не используется — кладём заглушку
  y.set_size(X.n_rows, 1);
  y.fill(0.0);

  // 3) Сохраним кэш (best-effort)
  try {
    fs::create_directories("cache/xy");
    X.save(xfile, arma::csv_ascii);
    y.save(yfile, arma::csv_ascii);
  } catch (...) {
    // некритично
  }
  return true;
}

nlohmann::json data_health_report(const std::string& symbol,
                                  const std::string& interval)
{
  json out = json::object();
  const std::string p_clean = "cache/clean/" + upper(symbol) + "_" + norm_interval(interval) + ".csv";
  const std::string p_raw   = "cache/"       + upper(symbol) + "_" + norm_interval(interval) + ".csv";

  auto info = [&](const std::string& p){
    json j = json::object();
    j["exists"] = fs::exists(p);
    if(j["exists"].get<bool>()){
      // дешёво — посчитаем кол-во колонок в первой строке и строк всего
      size_t rows = 0;
      size_t cols = 0;
      std::ifstream f(p);
      std::string line;
      if(std::getline(f,line)){
        std::stringstream ss(line); std::string cell;
        while(std::getline(ss,cell,',')) ++cols;
        rows = 1;
        while(std::getline(f,line)) ++rows;
      }
      j["rows"] = (long long)rows;
      j["cols"] = (long long)cols;
      j["path"] = p;
    }
    return j;
  };

  out["symbol"]   = upper(symbol);
  out["interval"] = norm_interval(interval);
  out["clean"]    = info(p_clean);
  out["raw"]      = info(p_raw);

  bool ok6 = false, okN = false;
  if(out["clean"].value("exists", false)){
    ok6 = (out["clean"].value("cols", 0) == 6);
    okN = (out["clean"].value("rows", 0) >= 300);
  } else if(out["raw"].value("exists", false)){
    ok6 = (out["raw"].value("cols", 0) >= 6); // сможем урезать
    okN = (out["raw"].value("rows", 0) >= 300);
  }
  out["ok"] = ok6 && okN;
  return out;
}

nlohmann::json get_data_health(){
  const std::string sym = "BTCUSDT";
  json j = json::object();
  j["ok"] = true;
  j["data"] = json::array({
    data_health_report(sym, "15"),
    data_health_report(sym, "60"),
    data_health_report(sym, "240"),
    data_health_report(sym, "1440")
  });
  return j;
}

} // namespace etai
