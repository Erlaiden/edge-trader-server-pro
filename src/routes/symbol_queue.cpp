#include "routes/symbol_queue.h"

#include <atomic>
#include <condition_variable>
#include <chrono>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>

#include "json.hpp"
#include "utils_data.h"   // etai::backfill_last_months

using json = nlohmann::json;
using Clock = std::chrono::system_clock;

namespace etaiq {

struct Task {
    unsigned long long id;
    std::string symbol;
    std::string interval;
    int months;

    std::atomic<State> state{State::QUEUED};
    Clock::time_point created_at{Clock::now()};
    Clock::time_point started_at{};
    Clock::time_point finished_at{};

    json backfill;         // результат etai::backfill_last_months
    std::string error;     // текст ошибки
};

static std::mutex g_mu;
static std::condition_variable g_cv;
static std::queue<unsigned long long> g_q;
static std::unordered_map<unsigned long long, std::shared_ptr<Task>> g_tasks;

static std::atomic<bool> g_worker_started{false};
static std::atomic<unsigned long long> g_next_id{1};

// Метрики
static std::atomic<unsigned long long> g_enqueued_total{0};
static std::atomic<unsigned long long> g_succeeded_total{0};
static std::atomic<unsigned long long> g_failed_total{0};
static std::atomic<unsigned long long> g_running{0};

static std::string iso(Clock::time_point tp) {
    if (tp.time_since_epoch().count() == 0) return std::string();
    std::time_t t = Clock::to_time_t(tp);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return std::string(buf);
}

static void worker_loop() {
    for (;;) {
        unsigned long long id = 0ULL;
        {
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait(lk, []{ return !g_q.empty(); });
            id = g_q.front();
            g_q.pop();
        }

        std::shared_ptr<Task> t;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            auto it = g_tasks.find(id);
            if (it == g_tasks.end()) continue;
            t = it->second;
        }

        t->state = State::RUNNING;
        t->started_at = Clock::now();
        g_running.fetch_add(1);

        try {
            t->backfill = etai::backfill_last_months(t->symbol, t->interval, t->months);
            bool ok = t->backfill.value("ok", false);
            if (ok) {
                t->state = State::DONE;
                g_succeeded_total.fetch_add(1);
            } else {
                t->state = State::FAILED;
                t->error = t->backfill.value("error", std::string("unknown_error"));
                g_failed_total.fetch_add(1);
            }
        } catch (const std::exception& e) {
            t->state = State::FAILED;
            t->error = e.what();
            g_failed_total.fetch_add(1);
        } catch (...) {
            t->state = State::FAILED;
            t->error = "unknown_exception";
            g_failed_total.fetch_add(1);
        }

        t->finished_at = Clock::now();
        g_running.fetch_sub(1);
    }
}

void ensure_worker() {
    bool expected = false;
    if (g_worker_started.compare_exchange_strong(expected, true)) {
        std::thread(worker_loop).detach();
    }
}

unsigned long long enqueue(const std::string& symbol, const std::string& interval, int months) {
    auto task = std::make_shared<Task>();
    task->id = g_next_id.fetch_add(1);
    task->symbol = symbol;
    task->interval = interval;
    task->months = months;

    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_tasks.emplace(task->id, task);
        g_q.push(task->id);
    }
    g_enqueued_total.fetch_add(1);
    g_cv.notify_one();
    return task->id;
}

json status(unsigned long long id) {
    std::shared_ptr<Task> t;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_tasks.find(id);
        if (it == g_tasks.end()) {
            return json{{"ok", false}, {"error", "task_not_found"}, {"id", id}};
        }
        t = it->second;
    }

    std::string state_str = "queued";
    switch (t->state.load()) {
        case State::QUEUED:  state_str = "queued"; break;
        case State::RUNNING: state_str = "running"; break;
        case State::DONE:    state_str = "done"; break;
        case State::FAILED:  state_str = "failed"; break;
    }

    json j{
        {"ok", true},
        {"id", t->id},
        {"state", state_str},
        {"symbol", t->symbol},
        {"interval", t->interval},
        {"months", t->months},
        {"created_at", iso(t->created_at)},
        {"started_at", iso(t->started_at)},
        {"finished_at", iso(t->finished_at)},
    };
    if (!t->error.empty()) j["error"] = t->error;
    if (!t->backfill.is_null()) j["backfill"] = t->backfill;
    return j;
}

json metrics_json() {
    std::lock_guard<std::mutex> lk(g_mu);
    return json{
        {"enqueued_total", g_enqueued_total.load()},
        {"running",        g_running.load()},
        {"succeeded_total",g_succeeded_total.load()},
        {"failed_total",   g_failed_total.load()},
        {"queue_length",   static_cast<unsigned long long>(g_q.size())}
    };
}

} // namespace etaiq
