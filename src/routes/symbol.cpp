#include "symbol_queue.h"
#include "utils.h"
#include "http_helpers.h"
#include <httplib.h>
#include <unordered_set>
#include <optional>
#include <vector>

using json = nlohmann::json;

namespace etai {

namespace {

const std::unordered_set<std::string> kAllowedIntervals{"15","60","240","1440"};

std::string normalize_symbol(const std::string& raw) {
  std::string s;
  s.reserve(raw.size());
  for (unsigned char c : raw) {
    if (std::isspace(c)) continue;
    s.push_back(static_cast<char>(std::toupper(c)));
  }
  if (s.empty()) return "BTCUSDT";
  return s;
}

std::optional<std::string> normalize_interval(const std::string& raw) {
  std::string canon = canonical_interval(raw);
  if (canon.empty()) return std::nullopt;
  if (kAllowedIntervals.count(canon) == 0) return std::nullopt;
  return canon;
}

int clamp_months(int months) {
  if (months < 1) return 1;
  if (months > 36) return 36;
  return months;
}

json error_json(const std::string& err, const std::string& detail = {}) {
  json j{{"ok", false}, {"error", err}};
  if (!detail.empty()) j["error_detail"] = detail;
  return j;
}

std::vector<json> build_task_requests(const json& payload, const httplib::Request& req) {
  std::vector<json> tasks;
  if (payload.contains("tasks") && payload["tasks"].is_array()) {
    for (const auto& item : payload["tasks"]) {
      if (!item.is_object()) continue;
      json task = item;
      if (!task.contains("interval") && item.contains("tf")) {
        task["interval"] = item["tf"];
      }
      tasks.push_back(task);
    }
  } else if (payload.contains("intervals") && payload["intervals"].is_array()) {
    int def_months = payload.value("months", 6);
    for (const auto& val : payload["intervals"]) {
      if (val.is_string()) {
        tasks.push_back(json{{"interval", val.get<std::string>()}, {"months", def_months}});
      }
    }
  } else if (payload.contains("interval") && payload["interval"].is_string()) {
    tasks.push_back(json{{"interval", payload["interval"].get<std::string>()}, {"months", payload.value("months", 6)}});
  }

  if (tasks.empty()) {
    std::string interval_query = qp(req, "interval", "");
    if (!interval_query.empty()) {
      int m = 6;
      try { m = std::stoi(qp(req, "months", "6")); } catch (...) {}
      tasks.push_back(json{{"interval", interval_query}, {"months", m}});
    }
  }
  return tasks;
}

} // namespace

void register_symbol_routes(httplib::Server& svr) {
  svr.Post("/api/symbol/hydrate", [](const httplib::Request& req, httplib::Response& res){
    json payload;
    if (!req.body.empty()) {
      payload = json::parse(req.body, nullptr, false);
      if (payload.is_discarded()) {
        auto err = error_json("bad_json", "Failed to parse request body");
        res.status = 400;
        res.set_content(err.dump(2), "application/json");
        return;
      }
    } else {
      payload = json::object();
    }

    const std::string symbol = normalize_symbol(payload.value("symbol", qp(req, "symbol", "BTCUSDT")));
    auto tasks_json = build_task_requests(payload, req);
    if (tasks_json.empty()) {
      auto err = error_json("no_tasks", "No intervals provided");
      res.status = 400;
      res.set_content(err.dump(2), "application/json");
      return;
    }

    SymbolHydrateQueue& queue = SymbolHydrateQueue::instance();
    std::vector<json> enqueued;
    enqueued.reserve(tasks_json.size());

    for (const auto& task : tasks_json) {
      if (!task.contains("interval")) continue;
      std::string interval_raw = task["interval"].get<std::string>();
      auto norm = normalize_interval(interval_raw);
      if (!norm) {
        auto err = error_json("invalid_interval", interval_raw);
        res.status = 400;
        res.set_content(err.dump(2), "application/json");
        return;
      }
      int months = clamp_months(task.value("months", 6));
      std::string task_id = queue.enqueue(symbol, *norm, months);
      auto snapshot = queue.snapshot_task(task_id);
      if (snapshot) {
        enqueued.push_back(snapshot->to_json());
      }
    }

    json out{
      {"ok", true},
      {"symbol", symbol},
      {"tasks", enqueued}
    };
    res.set_content(out.dump(2), "application/json");
  });

  svr.Get("/api/symbol/status", [](const httplib::Request& req, httplib::Response& res){
    std::string symbol = qp(req, "symbol", "");
    std::string interval = qp(req, "interval", "");
    SymbolHydrateQueue& queue = SymbolHydrateQueue::instance();
    auto snaps = queue.snapshot_symbol(symbol, interval);
    json out{{"ok", true}, {"tasks", json::array()}};
    for (const auto& snap : snaps) {
      out["tasks"].push_back(snap.to_json());
    }
    res.set_content(out.dump(2), "application/json");
  });

  svr.Get("/api/symbol/task", [](const httplib::Request& req, httplib::Response& res){
    if (!req.has_param("id")) {
      auto err = error_json("missing_id");
      res.status = 400;
      res.set_content(err.dump(2), "application/json");
      return;
    }
    const std::string id = req.get_param_value("id");
    SymbolHydrateQueue& queue = SymbolHydrateQueue::instance();
    auto snap = queue.snapshot_task(id);
    if (!snap) {
      auto err = error_json("task_not_found");
      res.status = 404;
      res.set_content(err.dump(2), "application/json");
      return;
    }
    json out{{"ok", true}, {"task", snap->to_json()}};
    res.set_content(out.dump(2), "application/json");
  });

  svr.Get("/api/symbol/metrics", [](const httplib::Request& req, httplib::Response& res){
    (void)req;
    SymbolHydrateQueue& queue = SymbolHydrateQueue::instance();
    auto metrics = queue.metrics_json();
    res.set_content(metrics.dump(2), "application/json");
  });
}

} // namespace etai
