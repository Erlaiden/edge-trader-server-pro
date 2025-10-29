#pragma once
#include "httplib.h"
#include "json.hpp"
#include "http_helpers.h"   // qp()
#include "utils.h"          // etai::backfill_last_months
#include "utils_data.h"     // data_health_report()
#include "rt_metrics.h"     // REQ_BACKFILL
#include <set>
#include <string>

using json = nlohmann::json;

// /api/backfill?symbol=BTCUSDT&months=1&which=15,60,240,1440
static inline void register_backfill_routes(httplib::Server& srv){
  srv.Get("/api/backfill", [](const httplib::Request& req, httplib::Response& res){
    REQ_BACKFILL.fetch_add(1, std::memory_order_relaxed);

    const std::string symbol = qp(req, "symbol", "BTCUSDT");
    const int months = [&]{
      try { return std::max(1, std::stoi(qp(req,"months","1"))); } catch(...) { return 1; }
    }();

    std::set<std::string> wanted = {"15","60","240","1440"};
    {
      const auto which = qp(req, "which", "");
      if(!which.empty()){
        wanted.clear();
        std::string acc;
        for(char c: which){
          if(c==','){ if(!acc.empty()) { wanted.insert(acc); acc.clear(); } }
          else acc.push_back(c);
        }
        if(!acc.empty()) wanted.insert(acc);
      }
    }

    json intervals = json::array();
    json health    = json::array();

    auto do_one = [&](const std::string& tf){
      auto r = etai::backfill_last_months(symbol, tf, months);
      intervals.push_back({
        {"symbol",symbol},{"interval",tf},{"months",months},
        {"ok", r.value("ok", false)}, {"rows", r.value("rows", 0)}
      });
      auto h = data_health_report(symbol, tf);
      h["interval"] = tf; h["symbol"] = symbol;
      health.push_back(h);
    };

    for (auto& tf: wanted){
      if (tf=="15"||tf=="60"||tf=="240"||tf=="1440") do_one(tf);
    }

    json out{
      {"ok", true},
      {"intervals", intervals},
      {"health", health}
    };
    res.set_content(out.dump(2), "application/json");
  });
}
