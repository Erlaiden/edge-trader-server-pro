#pragma once
#include "../httplib.h"

namespace etai {
// Регистрирует маршрут /api/agents/decision.
// Активируется только если выставлен ETAI_AGENT_ENABLE=1.
void setup_agents_routes(httplib::Server& svr);
} // namespace etai
