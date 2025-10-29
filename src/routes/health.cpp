// Minimal dispatcher for health/backfill/train (+ AI health via separate TU)
#include "health.h"
#include "httplib.h"
#include "json.hpp"

// ВКЛЮЧАЕМ только базовые инклюд-юниты, которые задумывались как inline-вставка:
#include "routes/health_base.inc.cpp"  // /health (инициализация атомиков из кеша)
#include "routes/backfill.inc.cpp"     // /api/backfill
#include "routes/train.inc.cpp"        // /api/train (PRO)

// Форвард-декларация: реализация находится в src/routes/health_ai.cpp
void register_health_ai(httplib::Server&);

// Единая точка регистрации роутов хелса и смежных штук
void register_health_routes(httplib::Server& srv) {
  register_health_base(srv);
  register_health_ai(srv);     // /api/health/ai и /api/health/ai/extended
  register_backfill_routes(srv);
  register_train_routes(srv);
}
