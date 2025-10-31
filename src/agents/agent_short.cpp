#include "agent_base.h"
#include <cmath>
#include <armadillo>

namespace etai {

// Агент Short: реагирует на отрицательные сигналы признаков.
// Логика: если отрицательная энергия (инвертированная норма) > thr → short (−1)
class AgentShort : public AgentBase {
public:
    AgentShort() = default;
    ~AgentShort() override = default;

    int decide(const arma::rowvec& features, double thr) override {
        double energy = arma::mean(arma::abs(features));
        last_confidence = 1.0 / (1.0 + std::exp(-energy));
        return (last_confidence > thr) ? -1 : 0;
    }

    std::string name() const override {
        return "agent_short";
    }
};

// Фабричная функция
extern "C" AgentBase* create_agent_short() {
    return new AgentShort();
}

}  // namespace etai
