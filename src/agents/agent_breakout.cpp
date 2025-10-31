#include <armadillo>
#include <cmath>
#include <memory>
#include "agent_base.h"
#include "agents_factory.h"

namespace etai {

class AgentBreakout : public AgentBase {
public:
    std::string name() const override { return "breakout"; }

    int decide(const arma::rowvec& features, double thr) override {
        const std::size_t n = features.n_cols;
        if (n < 4) { last_confidence = 0.0; return 0; }
        double trend = features[n-1] - features[n-4];
        double vol = arma::stddev(features);
        last_confidence = std::tanh(vol + std::abs(trend));
        if (last_confidence > thr) return trend >= 0 ? +1 : -1;
        return 0;
    }
};

std::unique_ptr<AgentBase> make_agent_breakout() {
    return std::make_unique<AgentBreakout>();
}

} // namespace etai
