#include "agents.h"
#include "json.hpp"
#include "rt_metrics.h"

using json = nlohmann::json;

void register_agents_routes(httplib::Server& srv){
  // Просмотр агрегатов агентов
  srv.Get("/api/agents", [&](const httplib::Request&, httplib::Response& res){
    json out{
      {"ok", true},
      {"long_total",   INFER_SIG_LONG.load(std::memory_order_relaxed)},
      {"short_total",  INFER_SIG_SHORT.load(std::memory_order_relaxed)},
      {"neutral_total",INFER_SIG_NEUTRAL.load(std::memory_order_relaxed)},
      {"last_infer_ts_ms", LAST_INFER_TS.load(std::memory_order_relaxed)}
    };
    res.set_content(out.dump(), "application/json");
  });

  // Сброс агрегатов агентов
  srv.Post("/api/agents/reset", [&](const httplib::Request&, httplib::Response& res){
    json before{
      {"long_total",   INFER_SIG_LONG.load(std::memory_order_relaxed)},
      {"short_total",  INFER_SIG_SHORT.load(std::memory_order_relaxed)},
      {"neutral_total",INFER_SIG_NEUTRAL.load(std::memory_order_relaxed)}
    };

    INFER_SIG_LONG.store(0, std::memory_order_relaxed);
    INFER_SIG_SHORT.store(0, std::memory_order_relaxed);
    INFER_SIG_NEUTRAL.store(0, std::memory_order_relaxed);

    json after{
      {"long_total",   INFER_SIG_LONG.load(std::memory_order_relaxed)},
      {"short_total",  INFER_SIG_SHORT.load(std::memory_order_relaxed)},
      {"neutral_total",INFER_SIG_NEUTRAL.load(std::memory_order_relaxed)}
    };

    json out{
      {"ok", true},
      {"before", before},
      {"after", after},
      {"last_infer_ts_ms", LAST_INFER_TS.load(std::memory_order_relaxed)}
    };
    res.set_content(out.dump(), "application/json");
  });
}
