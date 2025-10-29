#pragma once
#include "httplib.h"

// Роуты для счётчиков агентов инференса:
//   GET  /api/agents        -> { long_total, short_total, neutral_total, last_infer_ts_ms }
//   POST /api/agents/reset  -> обнуление счётчиков
void register_agents_routes(httplib::Server& srv);
