#pragma once
#include "json.hpp"
#include "utils.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace httplib {
class Server;
}

namespace etai {

struct TaskSnapshot {
  std::string id;
  std::string symbol;
  std::string interval;
  int months = 0;
  std::string state;
  std::string error;
  BackfillStats backfill;
  std::uint64_t enqueued_at = 0;
  std::uint64_t started_at = 0;
  std::uint64_t finished_at = 0;

  nlohmann::json to_json() const;
};

class SymbolHydrateQueue {
public:
  using Executor = std::function<BackfillStats(const std::string&, const std::string&, int)>;

  static SymbolHydrateQueue& instance();

  std::string enqueue(const std::string& symbol, const std::string& interval, int months);
  std::vector<TaskSnapshot> snapshot_all() const;
  std::vector<TaskSnapshot> snapshot_symbol(const std::string& symbol, const std::string& interval = {}) const;
  std::optional<TaskSnapshot> snapshot_task(const std::string& id) const;
  nlohmann::json metrics_json() const;

  void set_executor(Executor exec);
  void wait_for_idle() const;
  void clear_for_tests();

private:
  SymbolHydrateQueue();
  ~SymbolHydrateQueue();
  SymbolHydrateQueue(const SymbolHydrateQueue&) = delete;
  SymbolHydrateQueue& operator=(const SymbolHydrateQueue&) = delete;

  struct Task {
    std::string id;
    std::string symbol;
    std::string interval;
    int months = 0;
    std::string state;
    std::string error;
    BackfillStats backfill;
    std::uint64_t enqueued_at = 0;
    std::uint64_t started_at = 0;
    std::uint64_t finished_at = 0;
  };

  class RunningGuard {
  public:
    explicit RunningGuard(SymbolHydrateQueue& q);
    ~RunningGuard();
  private:
    SymbolHydrateQueue& queue_;
    bool active_ = false;
  };

  void worker_loop();
  TaskSnapshot snapshot_locked(const std::shared_ptr<Task>& task) const;
  static std::uint64_t now_ms();

  mutable std::mutex mutex_;
  mutable std::condition_variable cv_;
  mutable std::condition_variable idle_cv_;
  bool stop_ = false;
  std::thread worker_;
  std::deque<std::shared_ptr<Task>> queue_;
  std::unordered_map<std::string, std::shared_ptr<Task>> tasks_;
  std::atomic<std::uint64_t> id_counter_{0};
  std::atomic<size_t> enqueued_total_{0};
  std::atomic<size_t> running_{0};
  std::atomic<size_t> succeeded_total_{0};
  std::atomic<size_t> failed_total_{0};
  Executor executor_;
};

void register_symbol_routes(httplib::Server& svr);

} // namespace etai
