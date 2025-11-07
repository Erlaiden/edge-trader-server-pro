#include "routes/symbol_queue.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>

#include "json.hpp"
#include "utils.h"        // parse utils, join_csv (для совместимости включён)
#include "utils_data.h"   // etai::backfill_last_months

using json = nlohmann::json;
using Clock = std::chrono::system_clock;

namespace etaiq {

namespace {

struct Task {
  unsigned long long id{0};
  std::string symbol;
  std::string interval;
  int months{0};

  mutable std::mutex mu;
  State state{State::QUEUED};
  Clock::time_point created_at{Clock::now()};
  Clock::time_point started_at{};
  Clock::time_point finished_at{};
  json backfill{json{}};
  std::string error;
};

struct TaskSnapshot {
  unsigned long long id{0};
  std::string symbol;
  std::string interval;
  int months{0};
  State state{State::QUEUED};
  Clock::time_point created_at{};
  Clock::time_point started_at{};
  Clock::time_point finished_at{};
  json backfill;
  std::string error;
};

class RunningGuard {
public:
  explicit RunningGuard(std::atomic<unsigned long long>& c) : counter_(c), active_(true) {}
  RunningGuard(const RunningGuard&) = delete;
  RunningGuard& operator=(const RunningGuard&) = delete;
  ~RunningGuard() { release(); }
  void release() {
    if (active_) { counter_.fetch_sub(1, std::memory_order_relaxed); active_ = false; }
  }
private:
  std::atomic<unsigned long long>& counter_;
  bool active_;
};

std::mutex g_mu;
std::condition_variable g_cv;
std::queue<unsigned long long> g_queue;
std::unordered_map<unsigned long long, std::shared_ptr<Task>> g_tasks;
std::atomic<bool> g_worker_started{false};
std::atomic<unsigned long long> g_next_id{1};

std::atomic<unsigned long long> g_enqueued_total{0};
std::atomic<unsigned long long> g_succeeded_total{0};
std::atomic<unsigned long long> g_failed_total{0};
std::atomic<unsigned long long> g_running{0};

std::string to_state_string(State s) {
  switch (s) {
    case State::QUEUED:  return "queued";
    case State::RUNNING: return "running";
    case State::DONE:    return "done";
    case State::FAILED:  return "failed";
  }
  return "queued";
}

std::string iso(Clock::time_point tp) {
  if (tp.time_since_epoch().count() == 0) return {};
  std::time_t time = Clock::to_time_t(tp);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &time);
#else
  gmtime_r(&time, &tm);
#endif
  char buf[32];
  if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) return {};
  return std::string(buf);
}

TaskSnapshot snapshot_task(const std::shared_ptr<Task>& t) {
  TaskSnapshot s;
  std::lock_guard<std::mutex> lk(t->mu);
  s.id = t->id;
  s.symbol = t->symbol;
  s.interval = t->interval;
  s.months = t->months;
  s.state = t->state;
  s.created_at = t->created_at;
  s.started_at = t->started_at;
  s.finished_at = t->finished_at;
  s.backfill = t->backfill;
  s.error = t->error;
  return s;
}

json normalise_backfill(const json& raw, bool success, const std::string& err) {
  json r = json::object();
  if (raw.is_object()) r = raw;
  else if (!raw.is_null()) r["payload"] = raw;

  if (!r.contains("rows")) r["rows"] = 0;
  if (!r.contains("skipped_rows")) r["skipped_rows"] = 0;

  if (success) {
    r["ok"] = true;
    if (!r.contains("error")) r["error"] = nullptr;
  } else {
    std::string msg = err;
    if (msg.empty() && r.contains("error") && r["error"].is_string()) msg = r["error"].get<std::string>();
    if (msg.empty()) msg = "unknown_error";
    r["ok"] = false;
    r["error"] = msg;
  }
  if (r.contains("running")) r["running"] = false;
  return r;
}

static unsigned get_timeout_sec() {
  const char* v = std::getenv("ETAI_BACKFILL_TIMEOUT_SEC");
  if (!v) return 300;
  try {
    int n = std::stoi(v);
    if (n < 30) n = 30;
    if (n > 3600) n = 3600;
    return (unsigned)n;
  } catch (...) { return 300; }
}

void worker_loop() {
  for (;;) {
    std::shared_ptr<Task> task;
    unsigned long long id = 0ULL;
    {
      std::unique_lock<std::mutex> lk(g_mu);
      g_cv.wait(lk, [] { return !g_queue.empty(); });
      id = g_queue.front();
      g_queue.pop();
      auto it = g_tasks.find(id);
      if (it == g_tasks.end()) continue;
      task = it->second;
    }

    {
      std::lock_guard<std::mutex> lk(task->mu);
      task->state = State::RUNNING;
      task->started_at = Clock::now();
      task->error.clear();
      task->backfill = json::object({{"ok", false}, {"running", true}, {"rows", 0}, {"skipped_rows", 0}});
    }

    std::cerr << "[queue] task " << id << " -> running" << std::endl;

    g_running.fetch_add(1, std::memory_order_relaxed);
    RunningGuard rg(g_running);

    json backfill_result;
    std::string error_message;
    bool success = false;

    try {
      const unsigned timeout_sec = get_timeout_sec();
      auto fut = std::async(std::launch::async, [task]() {
        return etai::backfill_last_months(task->symbol, task->interval, task->months);
      });

      if (fut.wait_for(std::chrono::seconds(timeout_sec)) == std::future_status::ready) {
        backfill_result = fut.get();
        success = backfill_result.value("ok", false);
        if (!success) error_message = backfill_result.value("error", std::string("unknown_error"));
      } else {
        error_message = "timeout";
        backfill_result = json::object({{"ok", false}, {"error", error_message}});
      }
    } catch (const std::exception& ex) {
      error_message = ex.what();
      backfill_result = json::object({{"ok", false}, {"error", error_message}});
    } catch (...) {
      error_message = "unknown_exception";
      backfill_result = json::object({{"ok", false}, {"error", error_message}});
    }

    {
      std::lock_guard<std::mutex> lk(task->mu);
      task->finished_at = Clock::now();
      task->backfill = normalise_backfill(backfill_result, success, error_message);

      if (success) {
        task->state = State::DONE;
        task->error.clear();
        g_succeeded_total.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "[queue] task " << id << " -> done" << std::endl;
      } else {
        task->state = State::FAILED;
        task->error = task->backfill.value("error", error_message);
        g_failed_total.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "[queue] task " << id << " -> failed: " << task->error << std::endl;
      }
    }

    rg.release();
  }
}

} // namespace

void ensure_worker() {
  bool expected = false;
  if (g_worker_started.compare_exchange_strong(expected, true)) {
    std::thread(worker_loop).detach();
  }
}

unsigned long long enqueue(const std::string& symbol,
                           const std::string& interval,
                           int months) {
  auto task = std::make_shared<Task>();
  task->id = g_next_id.fetch_add(1, std::memory_order_relaxed);
  task->symbol = symbol;
  task->interval = interval;
  task->months = months;
  task->created_at = Clock::now();
  task->backfill = json();

  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_tasks.emplace(task->id, task);
    g_queue.push(task->id);
  }

  g_enqueued_total.fetch_add(1, std::memory_order_relaxed);
  std::cerr << "[queue] task " << task->id << " queued" << std::endl;
  g_cv.notify_one();
  return task->id;
}

nlohmann::json status(unsigned long long id) {
  std::shared_ptr<Task> task;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_tasks.find(id);
    if (it == g_tasks.end()) {
      return json{{"ok", false}, {"error", "task_not_found"}, {"id", id}};
    }
    task = it->second;
  }

  TaskSnapshot s = snapshot_task(task);

  json resp{
    {"ok", true},
    {"id", s.id},
    {"state", to_state_string(s.state)},
    {"symbol", s.symbol},
    {"interval", s.interval},
    {"months", s.months},
    {"created_at", iso(s.created_at)},
    {"started_at", iso(s.started_at)},
    {"finished_at", iso(s.finished_at)}
  };

  if (!s.error.empty()) resp["error"] = s.error;

  if (s.state == State::QUEUED) {
    resp["backfill"] = nullptr;
  } else {
    json bf = s.backfill;
    if (bf.is_null() || !bf.is_object()) {
      bf = json::object({
        {"ok", s.state == State::DONE},
        {"rows", 0},
        {"skipped_rows", 0},
        {"error", s.state == State::DONE ? json(nullptr) : json("unknown_error")}
      });
    }
    if (s.state == State::RUNNING && !bf.contains("ok")) bf["ok"] = false;
    if (s.state == State::RUNNING && bf.contains("error") && bf["error"].is_null()) bf["error"] = json("in_progress");
    resp["backfill"] = bf;
  }

  return resp;
}

nlohmann::json metrics_json() {
  std::size_t qlen = 0;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    qlen = g_queue.size();
  }
  return json{
    {"enqueued_total", g_enqueued_total.load(std::memory_order_relaxed)},
    {"running",        g_running.load(std::memory_order_relaxed)},
    {"succeeded_total",g_succeeded_total.load(std::memory_order_relaxed)},
    {"failed_total",   g_failed_total.load(std::memory_order_relaxed)},
    {"queue_length",   static_cast<unsigned long long>(qlen)}
  };
}

} // namespace etaiq
