#include "health.h"
#include "httplib.h"
#include "json.hpp"

// отдельные модули:
#include "routes/health_base.inc.cpp"   // /health   (инициализация атомиков из кеша)
#include "routes/health_ai.inc.cpp"     // /api/health/ai  (расширенный отчёт)
#include "routes/backfill.inc.cpp"      // /api/backfill
#include "routes/train.inc.cpp"         // /api/train (PRO)

void register_health_routes(httplib::Server& srv){
  register_health_base(srv);
  register_health_ai(srv);
  register_backfill_routes(srv);
  register_train_routes(srv);
}
