#include <armadillo>
#include <cmath>
#include <memory>
#include "agent_base.h"
#include "agents_factory.h"

namespace etai {

class AgentFlat : public AgentBase {
public:
    std::string name() const override { return "flat"; }

    int decide(const arma::rowvec& features, double thr) override {
        const std::size_t n = features.n_cols;
        if (n == 0) { last_confidence = 0.0; return 0; }
        double mean = arma::mean(features);
        double var = arma::mean(arma::square(features - mean)) + 1e-8;
        last_confidence = 1.0 / (1.0 + var);
        return 0; // flat не даёт направление, влияет уверенностью
    }
};

std::unique_ptr<AgentBase> make_agent_flat() {
    return std::make_unique<AgentFlat>();
}

} // namespace etai
