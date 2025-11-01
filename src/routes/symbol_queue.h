#pragma once
#include <string>
#include "json.hpp"

namespace etaiq { // Edge Trader AI Queue

// Состояния задачи
enum class State { QUEUED=0, RUNNING=1, DONE=2, FAILED=3 };

// Гарантирует, что фоновой воркер запущен
void ensure_worker();

// Постановка задачи гидрации; возвращает task_id
unsigned long long enqueue(const std::string& symbol, const std::string& interval, int months);

// Статус по task_id (JSON с полями: ok, id, state, symbol, interval, months, started_at, finished_at, error, backfill)
nlohmann::json status(unsigned long long id);

// Метрики очереди (JSON с полями: enqueued_total, running, succeeded_total, failed_total, queue_length)
nlohmann::json metrics_json();

} // namespace etaiq
