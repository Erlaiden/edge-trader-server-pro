// routes/model.cpp — единый источник правды для модели
#include "routes/model.h"
#include "json.hpp"
#include "http_helpers.h"   // qp()
#include "server_accessors.h" // etai::get_current_model(), get_model_thr(), get_model_ma_len()
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>

using json = nlohmann::json;
namespace fs = std::filesystem;

// --- helpers ---
static inline std::string upper(std::string s){
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::toupper(c); });
  return s;
}

static inline std::string norm_interval(std::string s){
  // принимаем "15" или 15, возвращаем строку без кавычек и пробелов
  s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c){ return std::isspace(c) || c=='\"'; }), s.end());
  return s;
}

static inline std::string model_path_of(const std::string& symbol, const std::string& interval){
  return "cache/models/" + upper(symbol) + "_" + norm_interval(interval) + "_ppo_pro.json";
}

static inline json safe_read_json_file(const std::string& p){
  std::ifstream f(p);
  if(!f.good()) return json::object();
  try{ json j; f >> j; return j; } catch(...){ return json::object(); }
}

// Строим нормализованный объект модели на основе атомов и диска
static inline json make_model(double thr, long long ma, const json& disk){
  json m = json::object();
  m["best_thr"] = thr;
  m["ma_len"]   = ma;
  m["schema"]   = "ppo_pro_v1";
  m["mode"]     = "pro";

  if(disk.is_object()){
    if(disk.contains("policy"))        m["policy"]        = disk["policy"];
    if(disk.contains("policy_source")) m["policy_source"] = disk["policy_source"];
    if(disk.contains("symbol"))        m["symbol"]        = disk["symbol"];
    if(disk.contains("interval"))      m["interval"]      = disk["interval"];
    if(!m.contains("symbol") || !m.contains("interval")){
      // запасной путь: если файла нет полей, не ломаем контракт
      m["symbol"]   = disk.value("symbol",   "BTCUSDT");
      m["interval"] = disk.value("interval", "15");
    }
  }
  try { return json::parse(m.dump()); } catch (...) { return m; }
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

  // Нормализованный объект модели, согласованный с /api/health/ai
  srv.Get("/api/model", [&](const httplib::Request& req, httplib::Response& res){
    const std::string symbol   = qp(req, "symbol",   "BTCUSDT");
    const std::string interval = qp(req, "interval", "15");
    const std::string path     = model_path_of(symbol, interval);

    const double    thr = etai::get_model_thr();
    const long long ma  = etai::get_model_ma_len();
    const json      disk = fs::exists(path) ? safe_read_json_file(path) : json::object();

    json out = json::object();
    out["ok"]           = true;
    out["model"]        = make_model(thr, ma, disk);
    out["model_thr"]    = thr;
    out["model_ma_len"] = ma;
    out["path"]         = path;
    out["exists"]       = fs::exists(path);

    res.set_content(out.dump(2), "application/json");
  });
}
