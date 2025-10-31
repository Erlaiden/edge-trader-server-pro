#include "agent_base.h"
#include <armadillo>

namespace etai {

// Агент Flat: используется при нейтральном рынке или низкой уверенности модели.
class AgentFlat : public AgentBase {
public:
    AgentFlat() = default;
    ~AgentFlat() override = default;

    int decide(const arma::rowvec& features, double thr) override {
        double variance = arma::var(features);
        last_confidence = 1.0 / (1.0 + variance); // чем выше шум, тем ниже уверенность
        return 0; // всегда flat
    }

    std::string name() const override {
        return "agent_flat";
    }
};

// Фабричная функция
extern "C" AgentBase* create_agent_flat() {
    return new AgentFlat();
}

}  // namespace etai
