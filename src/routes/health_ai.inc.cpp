#pragma once
#include "rt_metrics.h"
#include "http_helpers.h"
#include "utils.h"        // minutes_of()
#include "utils_data.h"   // data_health_report()
#include "json.hpp"
#include <fstream>
#include <ctime>
#include <algorithm>

using json = nlohmann::json;

static inline void register_health_ai(httplib::Server& srv) {
  srv.Get("/api/health/ai", [](const httplib::Request& req, httplib::Response& res){
    std::string symbol   = qp(req, "symbol", "BTCUSDT");
    std::string interval = qp(req, "interval", "15");

    // Здоровье кешей на всех ТФ
    json h15   = data_health_report(symbol, "15");
    json h60   = data_health_report(symbol, "60");
    json h240  = data_health_report(symbol, "240");
    json h1440 = data_health_report(symbol, "1440");

    // Ленивая инициализация атомиков по данным (если пустые)
    if (DATA_ROWS_15.load()==0 && h15.value("rows",0)>0) {
      DATA_ROWS_15.store(h15.value("rows",0), std::memory_order_relaxed);
      DATA_ROWS_60.store(h60.value("rows",0), std::memory_order_relaxed);
      DATA_ROWS_240.store(h240.value("rows",0), std::memory_order_relaxed);
      DATA_ROWS_1440.store(h1440.value("rows",0), std::memory_order_relaxed);
    }

    const long long now_ms = (long long)time(nullptr)*1000;
    const long long ts_max_15 = (long long)h15.value("ts_max", 0LL);
    const long long fresh_ms = (ts_max_15>0) ? std::max(0LL, now_ms - ts_max_15) : 0LL;
    DATA_FRESH_MS.store(fresh_ms, std::memory_order_relaxed);

    // Попытка восстановить модельные агрегаты из файла модели (если есть)
    json model_info = json::object();
    const std::string model_path = "cache/models/" + symbol + "_" + interval + "_ppo_pro.json";
    {
      std::ifstream f(model_path);
      if (f) {
        f >> model_info;
        model_info["ok"] = true;
        model_info["path"] = model_path;
        model_info["mode"] = "pro";
        if (!model_info.contains("cv_effective_folds"))
          model_info["cv_effective_folds"] = (unsigned long long)model_info.value("cv_folds", 0);
      } else {
        model_info = json{{"ok",false},{"path",model_path}};
      }
    }

    // Обновляем атомики порогов/MA только если в модели есть значения
    double best_thr = model_info.value("best_thr", (double)MODEL_BEST_THR.load());
    long long ma_len = (long long)model_info.value("ma_len", (long long)MODEL_MA_LEN.load());
    MODEL_BEST_THR.store(best_thr, std::memory_order_relaxed);
    MODEL_MA_LEN.store(ma_len,     std::memory_order_relaxed);

    if (model_info.value("ok", false)) {
      if (CV_FOLDS.load()==0) {
        CV_FOLDS.store((unsigned long long)model_info.value("cv_folds", 0), std::memory_order_relaxed);
        auto is_m  = model_info.value("is",  json::object());
        auto oos_m = model_info.value("oos", json::object());
        CV_IS_SHARPE.store(is_m.value("sharpe", 0.0),          std::memory_order_relaxed);
        CV_OOS_SHARPE.store(oos_m.value("sharpe", 0.0),        std::memory_order_relaxed);
        CV_IS_EXPEC.store(is_m.value("expectancy", 0.0),       std::memory_order_relaxed);
        CV_OOS_EXPEC.store(oos_m.value("expectancy", 0.0),     std::memory_order_relaxed);
        CV_OOS_DD_MAX.store(oos_m.value("drawdown_max", 0.0),  std::memory_order_relaxed);
      }
      if (CV_EFFECTIVE_FOLDS.load()==0) {
        CV_EFFECTIVE_FOLDS.store(
          (unsigned long long)model_info.value("cv_effective_folds",
                                               model_info.value("cv_folds", 0)),
          std::memory_order_relaxed
        );
      }
      if (TRAIN_ROWS_USED.load()==0) {
        TRAIN_ROWS_USED.store((unsigned long long)model_info.value("train_rows_used", 0),
                              std::memory_order_relaxed);
      }
      if (MODEL_BUILD_TS.load()==0) {
        MODEL_BUILD_TS.store((long long)model_info.value("build_ts", 0LL),
                             std::memory_order_relaxed);
      }
    }

    json cv{
      {"folds",            (unsigned long long)CV_FOLDS.load()},
      {"effective_folds",  (unsigned long long)CV_EFFECTIVE_FOLDS.load()},
      {"is",  {{"sharpe",(double)CV_IS_SHARPE.load()}, {"expectancy",(double)CV_IS_EXPEC.load()}}},
      {"oos", {{"sharpe",(double)CV_OOS_SHARPE.load()},{"expectancy",(double)CV_OOS_EXPEC.load()},{"drawdown_max",(double)CV_OOS_DD_MAX.load()}}}
    };

    json out{
      {"ok", true},
      {"ts", now_ms},
      {"data", {{"15",h15},{"60",h60},{"240",h240},{"1440",h1440}}},
      {"data_fresh_ms", fresh_ms},
      {"model", model_info},
      {"cv", cv},
      {"model_thr",    best_thr},
      {"model_ma_len", ma_len}
    };
    res.set_content(out.dump(2), "application/json");
  });
}
