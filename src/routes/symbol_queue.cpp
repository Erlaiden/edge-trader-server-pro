#include "symbol_queue.h"
#include <httplib.h>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <cstdlib>
#include <fstream>

namespace etai {

namespace {
std::string upper_symbol(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::toupper(c); });
  return s;
}
}

nlohmann::json TaskSnapshot::to_json() const {
  nlohmann::json j{
    {"task_id", id},
    {"state", state},
    {"symbol", symbol},
    {"interval", interval},
    {"months", months},
    {"enqueued_at", enqueued_at},
    {"started_at", started_at},
    {"finished_at", finished_at},
    {"backfill", backfill.to_json()}
  };
  if (!error.empty()) {
    j["error"] = error;
  }
  return j;
}

SymbolHydrateQueue& SymbolHydrateQueue::instance() {
  static SymbolHydrateQueue inst;
  return inst;
}

SymbolHydrateQueue::SymbolHydrateQueue() {
  executor_ = [](const std::string& symbol, const std::string& interval, int months){
    return backfill_last_months(symbol, interval, months);
  };
  if (const char* env = std::getenv("EDGE_SYMBOL_QUEUE_FAKE")) {
    if (std::string(env) == "1") {
      executor_ = [](const std::string& symbol, const std::string& interval, int months) {
        BackfillStats stats;
        std::string symbol_upper = symbol;
        std::transform(symbol_upper.begin(), symbol_upper.end(), symbol_upper.begin(), [](unsigned char c){ return std::toupper(c); });
        std::string canon = canonical_interval(interval);
        if (canon.empty()) canon = interval;
        stats.symbol = symbol_upper;
        stats.interval = canon;
        stats.canonical_interval = canon;
        stats.months = months;
        ensure_dir("cache");
        ensure_dir("cache/clean");
        const std::string raw_path = cache_file(symbol_upper, canon);
        const std::string clean_path = std::string("cache/clean/") + symbol_upper + "_" + canon + ".csv";
        std::ofstream raw(raw_path, std::ios::trunc);
        std::ofstream clean(clean_path, std::ios::trunc);
        const size_t rows = 720;
        long long base = 1700000000000LL;
        for (size_t i = 0; i < rows; ++i) {
          long long ts = base + static_cast<long long>(i) * 60000;
          raw << ts << ",1,2,3,4,5,6\n";
          clean << ts << ",1,2,3,4,5\n";
        }
        stats.ok = true;
        stats.rows = rows;
        stats.fetched_rows = rows;
        stats.skipped_rows = 0;
        return stats;
      };
    }
  }
  worker_ = std::thread(&SymbolHydrateQueue::worker_loop, this);
}

SymbolHydrateQueue::~SymbolHydrateQueue() {
  {
    std::lock_guard<std::mutex> lk(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

std::uint64_t SymbolHydrateQueue::now_ms() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string SymbolHydrateQueue::enqueue(const std::string& symbol, const std::string& interval, int months) {
  auto task = std::make_shared<Task>();
  task->id = std::to_string(id_counter_.fetch_add(1, std::memory_order_relaxed) + 1);
  task->symbol = upper_symbol(symbol);
  std::string canon = canonical_interval(interval);
  bool valid = !canon.empty();
  task->interval = valid ? canon : interval;
  task->months = std::min(36, std::max(1, months));
  task->state = "queued";
  task->backfill.symbol = task->symbol;
  task->backfill.interval = task->interval;
  task->backfill.canonical_interval = valid ? canon : canonical_interval(task->interval);
  if (task->backfill.canonical_interval.empty()) {
    task->backfill.canonical_interval = task->interval;
  }
  task->backfill.months = task->months;
  task->backfill.ok = false;
  task->backfill.rows = 0;
  task->backfill.skipped_rows = 0;
  task->backfill.fetched_rows = 0;
  task->enqueued_at = now_ms();
  {
    std::lock_guard<std::mutex> lk(mutex_);
    tasks_[task->id] = task;
    if (valid) {
      queue_.push_back(task);
    } else {
      task->state = "failed";
      task->error = "invalid_interval";
      task->backfill.error = "invalid_interval";
      task->backfill.interval = interval;
      task->backfill.canonical_interval = canonical_interval(interval);
      task->finished_at = task->enqueued_at;
      failed_total_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  if (valid) {
    enqueued_total_.fetch_add(1, std::memory_order_relaxed);
    cv_.notify_one();
  }
  return task->id;
}

void SymbolHydrateQueue::set_executor(Executor exec) {
  std::lock_guard<std::mutex> lk(mutex_);
  executor_ = std::move(exec);
}

void SymbolHydrateQueue::wait_for_idle() const {
  std::unique_lock<std::mutex> lk(mutex_);
  idle_cv_.wait(lk, [this]{
    return queue_.empty() && running_.load(std::memory_order_relaxed) == 0;
  });
}

void SymbolHydrateQueue::clear_for_tests() {
  wait_for_idle();
  std::lock_guard<std::mutex> lk(mutex_);
  queue_.clear();
  tasks_.clear();
  enqueued_total_.store(0);
  running_.store(0);
  succeeded_total_.store(0);
  failed_total_.store(0);
  id_counter_.store(0);
}

TaskSnapshot SymbolHydrateQueue::snapshot_locked(const std::shared_ptr<Task>& task) const {
  TaskSnapshot snap;
  snap.id = task->id;
  snap.symbol = task->symbol;
  snap.interval = task->interval;
  snap.months = task->months;
  snap.state = task->state;
  snap.error = task->error;
  snap.backfill = task->backfill;
  snap.enqueued_at = task->enqueued_at;
  snap.started_at = task->started_at;
  snap.finished_at = task->finished_at;
  return snap;
}

std::vector<TaskSnapshot> SymbolHydrateQueue::snapshot_all() const {
  std::lock_guard<std::mutex> lk(mutex_);
  std::vector<TaskSnapshot> snaps;
  snaps.reserve(tasks_.size());
  for (const auto& kv : tasks_) {
    snaps.push_back(snapshot_locked(kv.second));
  }
  std::sort(snaps.begin(), snaps.end(), [](const TaskSnapshot& a, const TaskSnapshot& b){
    return a.enqueued_at < b.enqueued_at;
  });
  return snaps;
}

std::vector<TaskSnapshot> SymbolHydrateQueue::snapshot_symbol(const std::string& symbol, const std::string& interval) const {
  std::lock_guard<std::mutex> lk(mutex_);
  std::vector<TaskSnapshot> snaps;
  const std::string symbol_up = upper_symbol(symbol);
  const std::string interval_norm = interval.empty() ? std::string() : canonical_interval(interval);
  for (const auto& kv : tasks_) {
    const auto& task = kv.second;
    if (!symbol_up.empty() && task->symbol != symbol_up) continue;
    if (!interval_norm.empty() && task->interval != interval_norm) continue;
    snaps.push_back(snapshot_locked(task));
  }
  std::sort(snaps.begin(), snaps.end(), [](const TaskSnapshot& a, const TaskSnapshot& b){
    return a.enqueued_at < b.enqueued_at;
  });
  return snaps;
}

std::optional<TaskSnapshot> SymbolHydrateQueue::snapshot_task(const std::string& id) const {
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = tasks_.find(id);
  if (it == tasks_.end()) return std::nullopt;
  return snapshot_locked(it->second);
}

nlohmann::json SymbolHydrateQueue::metrics_json() const {
  nlohmann::json j;
  j["ok"] = true;
  j["enqueued_total"] = static_cast<std::uint64_t>(enqueued_total_.load(std::memory_order_relaxed));
  j["running"] = static_cast<std::uint64_t>(running_.load(std::memory_order_relaxed));
  j["succeeded_total"] = static_cast<std::uint64_t>(succeeded_total_.load(std::memory_order_relaxed));
  j["failed_total"] = static_cast<std::uint64_t>(failed_total_.load(std::memory_order_relaxed));
  {
    std::lock_guard<std::mutex> lk(mutex_);
    j["queue_length"] = static_cast<std::uint64_t>(queue_.size());
  }
  return j;
}

SymbolHydrateQueue::RunningGuard::RunningGuard(SymbolHydrateQueue& q) : queue_(q) {
  queue_.running_.fetch_add(1, std::memory_order_relaxed);
  active_ = true;
}

SymbolHydrateQueue::RunningGuard::~RunningGuard() {
  if (!active_) return;
  auto remaining = queue_.running_.fetch_sub(1, std::memory_order_relaxed) - 1;
  std::unique_lock<std::mutex> lk(queue_.mutex_);
  if (queue_.queue_.empty() && remaining == 0) {
    queue_.idle_cv_.notify_all();
  }
}

void SymbolHydrateQueue::worker_loop() {
  while (true) {
    std::shared_ptr<Task> task;
    Executor exec;
    {
      std::unique_lock<std::mutex> lk(mutex_);
      cv_.wait(lk, [this]{ return stop_ || !queue_.empty(); });
      if (stop_ && queue_.empty()) {
        break;
      }
      task = queue_.front();
      queue_.pop_front();
      task->state = "running";
      task->started_at = now_ms();
      exec = executor_;
    }

    std::cerr << "[symbol_queue] start task id=" << task->id
              << " symbol=" << task->symbol
              << " interval=" << task->interval
              << " months=" << task->months << std::endl;

    RunningGuard guard(*this);

    BackfillStats stats;
    try {
      stats = exec(task->symbol, task->interval, task->months);
    } catch (const std::exception& e) {
      stats.ok = false;
      stats.error = e.what();
      stats.symbol = task->symbol;
      stats.interval = task->interval;
      stats.canonical_interval = canonical_interval(task->interval);
      stats.months = task->months;
    } catch (...) {
      stats.ok = false;
      stats.error = "unknown_exception";
      stats.symbol = task->symbol;
      stats.interval = task->interval;
      stats.canonical_interval = canonical_interval(task->interval);
      stats.months = task->months;
    }

    {
      std::lock_guard<std::mutex> lk(mutex_);
      task->finished_at = now_ms();
      task->backfill = stats;
      task->backfill.symbol = task->symbol;
      task->backfill.interval = task->interval;
      task->backfill.canonical_interval = canonical_interval(task->interval);
      task->backfill.months = task->months;
      if (!stats.ok) {
        task->state = "failed";
        task->error = stats.error.empty() ? "backfill_failed" : stats.error;
        failed_total_.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "[symbol_queue] fail task id=" << task->id
                  << " error=" << task->error << std::endl;
      } else {
        task->state = "done";
        task->error.clear();
        task->backfill.ok = true;
        succeeded_total_.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "[symbol_queue] done task id=" << task->id
                  << " rows=" << task->backfill.rows
                  << " skipped=" << task->backfill.skipped_rows << std::endl;
      }
      if (queue_.empty() && running_.load(std::memory_order_relaxed) == 1) {
        idle_cv_.notify_all();
      }
    }
  }
  idle_cv_.notify_all();
}

} // namespace etai
