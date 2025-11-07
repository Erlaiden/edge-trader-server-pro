#pragma once
#include "server_utils.h"

static void register_model_routes(httplib::Server& srv){
  srv.Get("/api/model", [](const httplib::Request& req, httplib::Response& res){
    REQ_MODEL.fetch_add(1, std::memory_order_relaxed);
    std::string symbol   = qp(req, "symbol", "BTCUSDT");
    std::string interval = qp(req, "interval", "15");

    const std::string path = "cache/models/" + symbol + "_" + interval + "_ppo_pro.json";
    std::ifstream f(path);
    if (!f){
      json out{{"ok",false},{"error","model_not_found"},{"path",path}};
      res.set_content(out.dump(), "application/json");
      return;
    }
    json model; f >> model; model["ok"]=true; model["path"]=path; model["mode"]="pro";
    res.set_content(model.dump(2), "application/json");
  });
}
