#pragma once
#include "httplib.h"

// Регистрирует /api/model/* роуты:
// - GET /api/model/status?symbol=BTCUSDT&interval=15  -> краткий статус (существует ли, meta)
// - GET /api/model/read?symbol=BTCUSDT&interval=15    -> полный JSON модели с диска
void register_model_routes(httplib::Server& srv);
