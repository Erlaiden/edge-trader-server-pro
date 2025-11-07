#pragma once
#include <memory>
#include "agent_base.h"

namespace etai {
std::unique_ptr<AgentBase> make_agent_long();
std::unique_ptr<AgentBase> make_agent_short();
std::unique_ptr<AgentBase> make_agent_flat();
std::unique_ptr<AgentBase> make_agent_breakout();
std::unique_ptr<AgentBase> make_agent_correction();
} // namespace etai
