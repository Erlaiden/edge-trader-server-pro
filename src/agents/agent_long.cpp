#include <armadillo>
#include <cmath>
#include <memory>
#include "agent_base.h"
#include "agents_factory.h"

namespace etai {

class AgentLong : public AgentBase {
public:
    std::string name() const override { return "long"; }

    int decide(const arma::rowvec& features, double thr) override {
        const std::size_t n = features.n_cols;
        std::size_t k = n > 8 ? 8 : n;
        double s = 0.0;
        for (std::size_t i = 0; i < k; ++i) s += features[n - 1 - i];
        double energy = s / static_cast<double>(k);
        last_confidence = 1.0 / (1.0 + std::exp(-energy));
        return (last_confidence > thr) ? +1 : 0;
    }
};

std::unique_ptr<AgentBase> make_agent_long() {
    return std::make_unique<AgentLong>();
}

} // namespace etai
