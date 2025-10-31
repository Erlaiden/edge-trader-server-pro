#include "agent_base.h"
#include <cmath>
#include <armadillo>

namespace etai {

// Агент Long: реагирует на позитивные сигналы логрег-политики
// Логика: если sigmoid(dot(features, w)) > thr → long (+1)
class AgentLong : public AgentBase {
public:
    AgentLong() = default;
    ~AgentLong() override = default;

    int decide(const arma::rowvec& features, double thr) override {
        // Простая логика уверенности по скользящей энергии признаков
        double energy = arma::mean(arma::abs(features));
        last_confidence = 1.0 / (1.0 + std::exp(-energy));

        return (last_confidence > thr) ? +1 : 0;
    }

    std::string name() const override {
        return "agent_long";
    }
};

// Фабричная функция (экспортируемая)
extern "C" AgentBase* create_agent_long() {
    return new AgentLong();
}

}  // namespace etai
