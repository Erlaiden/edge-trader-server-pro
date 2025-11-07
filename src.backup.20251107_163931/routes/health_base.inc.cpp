#include "utils_data.h"
#pragma once
#include "rt_metrics.h"      // атомики и PROCESS_START_MS
#include "http_helpers.h"    // qp()
#include "utils.h"           // minutes_of()
#include "utils_data.h"      // etai::data_health_report()
#include "json.hpp"
#include <ctime>

using json = nlohmann::json;

// локальный хелпер: гарантируем, что атомики data_rows инициализированы реальными числами из кеша
static inline void ensure_data_rows_initialized(const std::string& symbol){
  auto r15   = etai::data_health_report(symbol, "15");
  auto r60   = etai::data_health_report(symbol, "60");
  auto r240  = etai::data_health_report(symbol, "240");
  auto r1440 = etai::data_health_report(symbol, "1440");

  // Если нули — устанавливаем из отчётов (safe, relaxed)
  if (DATA_ROWS_15.load()   == 0 && r15.value("rows",0)   > 0) DATA_ROWS_15.store(  r15.value("rows",0),   std::memory_order_relaxed);
  if (DATA_ROWS_60.load()   == 0 && r60.value("rows",0)   > 0) DATA_ROWS_60.store(  r60.value("rows",0),   std::memory_order_relaxed);
  if (DATA_ROWS_240.load()  == 0 && r240.value("rows",0)  > 0) DATA_ROWS_240.store( r240.value("rows",0),  std::memory_order_relaxed);
  if (DATA_ROWS_1440.load() == 0 && r1440.value("rows",0) > 0) DATA_ROWS_1440.store(r1440.value("rows",0), std::memory_order_relaxed);
}

// Регистрация /health
static inline void register_health_base(httplib::Server& srv) {
  srv.Get("/health", [](const httplib::Request& req, httplib::Response& res){
    REQ_HEALTH.fetch_add(1, std::memory_order_relaxed);

    // символьчик берём как и в других роутах по умолчанию BTCUSDT
    const std::string symbol = qp(req, "symbol", "BTCUSDT");
    ensure_data_rows_initialized(symbol);

    const long long now_ms = (long long)time(nullptr) * 1000;
    json out{
      {"ok", true},
      {"ts", now_ms},
      {"uptime_s", (now_ms - PROCESS_START_MS) / 1000},
      {"model_build_ts_ms", MODEL_BUILD_TS.load()},
      {"last_train_ts_ms",  LAST_TRAIN_TS.load()},
      {"last_infer_ts_ms",  LAST_INFER_TS.load()},
      {"data_rows", {
        {"15",   DATA_ROWS_15.load()},
        {"60",   DATA_ROWS_60.load()},
        {"240",  DATA_ROWS_240.load()},
        {"1440", DATA_ROWS_1440.load()}
      }}
    };
    res.set_content(out.dump(2), "application/json");
  });
}
