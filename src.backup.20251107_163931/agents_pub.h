#pragma once
#include <string>

namespace etai {

struct AgentPublic {
    bool        running{false};
    std::string symbol{"BTCUSDT"};
    std::string interval{"15"};
    std::string mode{"idle"};
};

// Возвращает «снимок» текущего состояния агента (для health/ai).
AgentPublic get_agent_public();

} // namespace etai
