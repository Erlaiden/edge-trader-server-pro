#include "routes/model.h"
#include "json.hpp"
#include "http_helpers.h"     // qp()
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>

#include "server_accessors.h" // etai::get_model_thr(), get_model_ma_len()
#include "utils_model.h"      // safe_read_json_file(), make_model()

using json = nlohmann::json;
namespace fs = std::filesystem;

// --- helpers ---
static inline std::string upper(std::string s){
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::toupper(c); });
  return s;
}

static inline std::string norm_interval(std::string s){
  s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c){ return std::isspace(c) || c=='\"'; }), s.end());
  return s;
}

static inline std::string model_path_of(const std::string& symbol, const std::string& interval){
  return "cache/models/" + upper(symbol) + "_" + norm_interval(interval) + "_ppo_pro.json";
}

// --- routes ---
void register_model_routes(httplib::Server& srv) {
  // Короткий статус
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

  // Полное чтение файла как есть
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

  // Нормализованный объект модели + инварианты
  srv.Get("/api/model", [&](const httplib::Request& req, httplib::Response& res){
    const std::string symbol   = qp(req, "symbol",   "BTCUSDT");
    const std::string interval = qp(req, "interval", "15");
    const std::string path     = model_path_of(symbol, interval);

    const double    thr = etai::get_model_thr();
    const long long ma  = etai::get_model_ma_len();
    const json      disk = fs::exists(path) ? safe_read_json_file(path) : json::object();

    json model = make_model(thr, ma, disk);

    // Инварианты
    const std::string expected_schema   = "ppo_pro_v1";
    const std::string expected_mode     = "pro";
    const int         expected_feat_dim = 8;

    bool ok = true;
    json notes = json::array();

    const std::string schema = model.value("schema", std::string());
    const std::string mode   = model.value("mode",   std::string());
    int feat_dim = 0;
    if(model.contains("policy") && model["policy"].is_object()){
      feat_dim = model["policy"].value("feat_dim", 0);
    }

    if(schema != expected_schema){ ok = false; notes.push_back("schema_mismatch"); }
    if(mode   != expected_mode)  { ok = false; notes.push_back("mode_mismatch"); }
    if(feat_dim != expected_feat_dim){ ok = false; notes.push_back("feat_dim_mismatch"); }

    json inv{
      {"ok", ok},
      {"expected", {
        {"schema", expected_schema},
        {"mode", expected_mode},
        {"feat_dim", expected_feat_dim}
      }},
      {"actual", {
        {"schema", schema},
        {"mode", mode},
        {"feat_dim", feat_dim}
      }}
    };
    if(!notes.empty()) inv["notes"] = notes;

    json out = json::object();
    out["ok"]           = true;
    out["model"]        = model;
    out["model_thr"]    = thr;
    out["model_ma_len"] = ma;
    out["path"]         = path;
    out["exists"]       = fs::exists(path);
    out["invariants"]   = inv;

    res.set_content(out.dump(2), "application/json");
  });
}
