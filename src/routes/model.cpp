#include "routes/model.h"
#include "json.hpp"
#include "http_helpers.h"   // qp()
#include <filesystem>
#include <fstream>

using json = nlohmann::json;
namespace fs = std::filesystem;

static inline std::string model_path_of(const std::string& symbol, const std::string& interval) {
  return "cache/models/" + symbol + "_" + interval + "_ppo_pro.json";
}

void register_model_routes(httplib::Server& srv) {
  // --- Короткий статус (как и раньше, оставляем поведение) ---
  srv.Get("/api/model/status", [&](const httplib::Request& req, httplib::Response& res){
    const std::string symbol   = qp(req, "symbol",   "BTCUSDT");
    const std::string interval = qp(req, "interval", "15");
    const std::string path     = model_path_of(symbol, interval);

    json out;
    out["ok"]     = true;
    out["exists"] = fs::exists(path);
    out["path"]   = path;

    if (out["exists"].get<bool>()) {
      try {
        std::ifstream f(path);
        json m = json::object();
        if (f) f >> m;

        out["version"]  = m.value("version", 0);
        out["schema"]   = m.value("schema",  "");
        out["build_ts"] = m.value("build_ts",(long long)0);
        out["ma_len"]   = m.value("ma_len", 0);
        out["best_thr"] = m.value("best_thr", 0.0);
        out["tp"]       = m.value("tp", 0.0);
        out["sl"]       = m.value("sl", 0.0);
      } catch (...) {
        out["exists"] = false;
      }
    }

    res.set_content(out.dump(2), "application/json");
  });

  // --- Полное чтение модели ---
  srv.Get("/api/model/read", [&](const httplib::Request& req, httplib::Response& res){
    const std::string symbol   = qp(req, "symbol",   "BTCUSDT");
    const std::string interval = qp(req, "interval", "15");
    const std::string path     = model_path_of(symbol, interval);

    if (!fs::exists(path)) {
      json out{{"ok", false}, {"error", "model_not_found"}, {"path", path}};
      res.set_content(out.dump(2), "application/json");
      return;
    }

    try {
      std::ifstream f(path);
      json m = json::object();
      if (f) f >> m;

      json out{
        {"ok", true},
        {"path", path},
        {"size_bytes", fs::file_size(path)},
        {"model", m}
      };
      res.set_content(out.dump(2), "application/json");
    } catch (const std::exception& e) {
      json out{{"ok", false}, {"error", e.what()}, {"path", path}};
      res.set_content(out.dump(2), "application/json");
    } catch (...) {
      json out{{"ok", false}, {"error", "unknown_exception"}, {"path", path}};
      res.set_content(out.dump(2), "application/json");
    }
  });
}
