#include "routes/symbol.h"
#include "routes/symbol_queue.h"

#include "http_helpers.h"
#include "json.hpp"
#include "utils.h"
#include "utils_data.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

inline std::string trim_upper(std::string s) {
  s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c){
    return std::isspace(c) || c=='"' || c=='\''; }), s.end());
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){
    return static_cast<char>(std::toupper(c)); });
  return s;
}

inline std::string normalize_symbol(std::string s) {
  s = trim_upper(std::move(s));
  return s.empty() ? std::string("BTCUSDT") : s;
}

inline std::string digits_only(std::string x) {
  x.erase(std::remove_if(x.begin(), x.end(), [](unsigned char c){ return !std::isdigit(c); }), x.end());
  return x;
}

inline std::string normalize_interval(std::string s) {
  s = trim_upper(std::move(s));
  if (s.empty()) return "15";
  const std::string d = digits_only(s);
  if (d.size() == s.size()) return d.empty() ? "15" : d;
  if (!d.empty()) {
    try {
      int base = std::stoi(d);
      if (base <= 0) return "15";
      const char unit = s.back();
      if (unit=='M') return std::to_string(base);
      if (unit=='H') return std::to_string(base*60);
      if (unit=='D') return std::to_string(base*1440);
    } catch (...) { return "15"; }
  }
  return "15";
}

inline int clamp_months(const std::string& mstr) {
  if (mstr.empty()) return 6;
  try {
    int m = std::stoi(mstr);
    if (m < 1) m = 1;
    if (m > 24) m = 24;
    return m;
  } catch (...) { return 6; }
}

inline bool interval_allowed(const std::string& tf) {
  static const std::set<std::string> allowed = {"15","60","240","1440"};
  return allowed.count(tf) > 0;
}

inline json allowed_intervals_json() {
  return json::array({"15","60","240","1440"});
}

} // namespace

void register_symbol_routes(httplib::Server& srv) {
  etaiq::ensure_worker();

  // Read-only статус данных
  srv.Get("/api/symbol/health", [](const httplib::Request& req, httplib::Response& res) {
    const std::string symbol   = normalize_symbol(qp(req, "symbol", "BTCUSDT"));
    const std::string interval = normalize_interval(qp(req, "interval", "15"));
    const int months           = clamp_months(qp(req, "months", "6"));

    json payload;
    if (!interval_allowed(interval)) {
      payload = {
        {"ok", false},
        {"error", "interval_not_allowed"},
        {"allowed", allowed_intervals_json()},
        {"symbol", symbol}, {"interval", interval}, {"months", months}
      };
      res.set_content(payload.dump(2), "application/json");
      return;
    }

    json health = etai::data_health_report(symbol, interval);
    payload = {
      {"ok", health.value("ok", false)},
      {"symbol", symbol},
      {"interval", interval},
      {"months", months},
      {"health", health}
    };
    res.set_content(payload.dump(2), "application/json");
  });

  // Неблокирующая постановка задачи гидрации
  srv.Post("/api/symbol/hydrate", [](const httplib::Request& req, httplib::Response& res) {
    const std::string symbol   = normalize_symbol(qp(req, "symbol", "BTCUSDT"));
    const std::string interval = normalize_interval(qp(req, "interval", "15"));
    const int months           = clamp_months(qp(req, "months", "6"));

    json payload;
    if (!interval_allowed(interval)) {
      payload = {
        {"ok", false},
        {"error", "interval_not_allowed"},
        {"allowed", allowed_intervals_json()},
        {"symbol", symbol}, {"interval", interval}, {"months", months}
      };
      res.set_content(payload.dump(2), "application/json");
      return;
    }

    const unsigned long long id = etaiq::enqueue(symbol, interval, months);
    payload = {
      {"ok", true},
      {"accepted", true},
      {"task_id", id},
      {"symbol", symbol},
      {"interval", interval},
      {"months", months}
    };
    res.status = 202;
    res.set_content(payload.dump(2), "application/json");
  });

  // Статус задачи
  srv.Get("/api/symbol/task", [](const httplib::Request& req, httplib::Response& res) {
    const std::string id_str = qp(req, "id", "");
    if (id_str.empty()) { res.set_content(json{{"ok", false}, {"error", "missing_id"}}.dump(2), "application/json"); return; }
    unsigned long long id = 0ULL;
    try { id = std::stoull(id_str); }
    catch (...) { res.set_content(json{{"ok", false}, {"error", "bad_id"}}.dump(2), "application/json"); return; }
    json payload = etaiq::status(id);
    res.set_content(payload.dump(2), "application/json");
  });

  // Метрики очереди
  srv.Get("/api/symbol/metrics", [](const httplib::Request&, httplib::Response& res) {
    try {
      res.set_content(etaiq::metrics_json().dump(2), "application/json");
    } catch (const std::exception& ex) {
      res.status = 500;
      res.set_content(json{{"ok", false}, {"error", ex.what()}}.dump(2), "application/json");
    } catch (...) {
      res.status = 500;
      res.set_content(json{{"ok", false}, {"error", "metrics_failure"}}.dump(2), "application/json");
    }
  });
}
