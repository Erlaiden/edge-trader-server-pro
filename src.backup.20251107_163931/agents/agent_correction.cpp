#include <armadillo>
#include <cmath>
#include <memory>
#include "agent_base.h"
#include "agents_factory.h"

namespace etai {

class AgentCorrection : public AgentBase {
public:
    std::string name() const override { return "correction"; }

    int decide(const arma::rowvec& features, double thr) override {
        const std::size_t n = features.n_cols;
        if (n < 2) { last_confidence = 0.0; return 0; }
        double momentum = features[n-1] - features[n-2];
        double magnitude = std::abs(momentum);
        last_confidence = std::tanh(magnitude);
        return 0; // только модифицирует уверенность
    }
};

std::unique_ptr<AgentBase> make_agent_correction() {
    return std::make_unique<AgentCorrection>();
}

} // namespace etai
