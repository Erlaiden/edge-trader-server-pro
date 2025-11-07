#pragma once
#include <armadillo>
#include <memory>
#include <vector>
#include <string>
#include <map>
#include "agent_base.h"

namespace etai {

struct AgentDecision {
    int signal = 0;            // +1 long, -1 short, 0 neutral
    double confidence = 0.0;   // [0..1]
};

struct AgentSummary {
    int final_signal = 0;
    double final_confidence = 0.0;
    double conflict_ratio = 0.0;
    std::map<std::string, AgentDecision> agents;
};

class AgentLayer {
public:
    AgentLayer();
    AgentSummary decide_all(const arma::rowvec& features, double thr);

private:
    std::vector<std::pair<std::string, std::unique_ptr<AgentBase>>> agents_;
};

} // namespace etai
