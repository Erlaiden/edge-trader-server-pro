#pragma once
#include "httplib.h"
#include "json.hpp"
#include "utils.h"
#include "fetch.h"
#include "ppo.h"
#include "ppo_pro.h"
#include "rt_metrics.h"
#include <armadillo>
#include <filesystem>
#include <ctime>
#include <set>
#include <string>
#include <optional>
#include <algorithm>
#include <fstream>

using json = nlohmann::json;
namespace fs = std::filesystem;

inline std::string qp(const httplib::Request& req, const char* key, const char* defv=nullptr){
  if (auto it = req.get_param_value(key, 0); !it.empty()) return it;
  return defv ? std::string(defv) : std::string();
}

inline int minutes_of(const std::string& interval){
  if (interval=="15") return 15;
  if (interval=="60") return 60;
  if (interval=="240") return 240;
  if (interval=="1440") return 1440;
  return 15;
}

inline json data_health_report(const std::string& symbol, const std::string& interval){
  arma::mat M = etai::load_cached_matrix(symbol, interval);
  json out{{"interval", interval},{"symbol",symbol},{"ok", M.n_elem>0},{"rows",(int)M.n_cols}};
  if (M.n_elem==0) return out;
  const arma::rowvec ts = M.row(0);
  int gaps=0, dups=0;
  long long ts_min=(long long)ts(0), ts_max=(long long)ts(M.n_cols-1);
  long long step_ms = (long long)minutes_of(interval) * 60LL * 1000LL;
  for (size_t i=1;i<M.n_cols;++i){
    long long dt = (long long)ts(i) - (long long)ts(i-1);
    if (dt>step_ms) gaps++; else if (dt==0) dups++;
  }
  out["gaps"]=gaps; out["dups"]=dups; out["ts_min"]=ts_min; out["ts_max"]=ts_max;
  return out;
}

// Тренировка PRO + сохранение модели + метрики
inline json run_train_pro_and_save(const std::string& symbol, const std::string& interval,
                                   int episodes, double tp, double sl, int ma_len){
  arma::mat M15 = etai::load_cached_matrix(symbol, interval);
  if (M15.n_elem==0){
    return json{{"ok",false},{"error","no_cached_data"},{"hint","call /api/backfill first"}};
  }
  arma::mat M60   = etai::load_cached_matrix(symbol, "60");
  arma::mat M240  = etai::load_cached_matrix(symbol, "240");
  arma::mat M1440 = etai::load_cached_matrix(symbol, "1440");
  const arma::mat *p60 = M60.n_elem? &M60:nullptr, *p240=M240.n_elem?&M240:nullptr, *p1440=M1440.n_elem?&M1440:nullptr;

  json metrics = etai::trainPPO_pro(M15, p60, p240, p1440, episodes, tp, sl, ma_len);
  metrics["version"] = metrics.value("version", 3);
  metrics["schema"]  = metrics.value("schema",  std::string("ppo_pro_v1"));

  json out{{"ok",metrics.value("ok",false)},{"symbol",symbol},{"interval",interval},
           {"episodes",episodes},{"tp",tp},{"sl",sl},{"metrics",metrics}};
  if (!out["ok"].get<bool>()) return out;

  fs::create_directories("cache/models");
  long long now_ms = (long long)time(nullptr)*1000;

  // CV агрегаты
  unsigned long long cv_folds = (unsigned long long)metrics.value("cv_folds", 0);
  unsigned long long cv_eff   = (unsigned long long)metrics.value("cv_effective_folds", (unsigned long long)cv_folds);
  json is_sum = metrics.value("is_summary", json::object());
  json oos_sum= metrics.value("oos_summary", json::object());

  CV_FOLDS.store(cv_folds, std::memory_order_relaxed);
  CV_EFFECTIVE_FOLDS.store(cv_eff, std::memory_order_relaxed);
  CV_IS_SHARPE.store(is_sum.value("sharpe", 0.0), std::memory_order_relaxed);
  CV_OOS_SHARPE.store(oos_sum.value("sharpe", 0.0), std::memory_order_relaxed);
  CV_IS_EXPEC.store(is_sum.value("expectancy", 0.0), std::memory_order_relaxed);
  CV_OOS_EXPEC.store(oos_sum.value("expectancy", 0.0), std::memory_order_relaxed);
  CV_OOS_DD_MAX.store(oos_sum.value("drawdown_max", 0.0), std::memory_order_relaxed);

  MODEL_BEST_THR.store(metrics.value("best_thr", 0.0), std::memory_order_relaxed);
  MODEL_MA_LEN.store((long long)metrics.value("ma_len", 12), std::memory_order_relaxed);

  // Модельный JSON
  json model{
    {"ok", true},
    {"ts", now_ms},
    {"symbol", symbol},
    {"interval", interval},
    {"mode","pro"},
    {"version", metrics.value("version",3)},
    {"schema",  metrics.value("schema","ppo_pro_v1")},
    {"build_ts", metrics.value("build_ts", now_ms)},
    {"ma_len", metrics.value("ma_len", 12)},
    {"best_thr", metrics.value("best_thr", 0.0)},
    {"tp", metrics.value("tp", 0.0)},
    {"sl", metrics.value("sl", 0.0)},
    {"episodes", metrics.value("episodes", episodes)},
    {"log_path", metrics.value("log_path","")},
    {"train_rows_total", metrics.value("train_rows_total", 0)},
    {"warmup_bars", metrics.value("warmup_bars", 0)},
    {"train_rows_used", metrics.value("train_rows_used", 0)},
    {"data_time_range", metrics.value("data_time_range", json::object())},
    {"split_index", metrics.value("split_index", 0)},
    {"cv_folds", cv_folds},
    {"cv_effective_folds", cv_eff},
    {"is", metrics.contains("is") ? metrics["is"] : metrics.value("is_summary", json::object())},
    {"oos", metrics.contains("oos") ? metrics["oos"] : metrics.value("oos_summary", json::object())}
  };
  const std::string path = "cache/models/" + symbol + "_" + interval + "_ppo_pro.json";
  { std::ofstream f(path); if (f) f << model.dump(2); }
  out["model_path"] = path;

  // Пром-метрики
  TRAINS_TOTAL.fetch_add(1, std::memory_order_relaxed);
  LAST_TRAIN_TS.store(now_ms, std::memory_order_relaxed);
  TRAIN_ROWS_USED.store(model.value("train_rows_used", 0), std::memory_order_relaxed);
  MODEL_BUILD_TS.store(model.value("build_ts", now_ms), std::memory_order_relaxed);

  return out;
}
