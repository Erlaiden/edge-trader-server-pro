#pragma once
#include "json.hpp"

namespace etai {
// Публичный статус агентного слоя для health/ai
nlohmann::json get_agent_public();
}
