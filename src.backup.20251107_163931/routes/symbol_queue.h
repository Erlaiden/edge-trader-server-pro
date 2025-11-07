#pragma once

#include <string>
#include "json.hpp"

namespace etaiq {

// Возможные состояния задачи в очереди гидрации символа.
enum class State : int { QUEUED = 0, RUNNING = 1, DONE = 2, FAILED = 3 };

// Гарантирует запуск фонового воркера обработки очереди.
void ensure_worker();

// Постановка задачи в очередь. Возвращает идентификатор задачи.
unsigned long long enqueue(const std::string& symbol,
                           const std::string& interval,
                           int months);

// Получение статуса задачи по её идентификатору.
// Формат JSON: { ok, id, state, symbol, interval, months, created_at, started_at, finished_at, error?, backfill? }
nlohmann::json status(unsigned long long id);

// Текущие метрики очереди (JSON: enqueued_total, running, succeeded_total, failed_total, queue_length)
nlohmann::json metrics_json();

} // namespace etaiq
