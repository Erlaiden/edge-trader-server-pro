#include "agent_base.h"
#include <armadillo>
#include <cmath>

namespace etai {

// Агент Breakout: реагирует на пробой уровней (высокая волатильность + направленность).
class AgentBreakout : public AgentBase {
public:
    AgentBreakout() = default;
    ~AgentBreakout() override = default;

    int decide(const arma::rowvec& features, double thr) override {
        // Волатильность и направленность
        double vol = arma::stddev(features);
        double trend = arma::mean(features);

        // Уверенность растёт с волатильностью и трендом
        last_confidence = std::tanh(vol + std::abs(trend));

        if (last_confidence > thr) {
            return (trend > 0) ? +1 : -1;
        }
        return 0;
    }

    std::string name() const override {
        return "agent_breakout";
    }
};

// Фабричная функция
extern "C" AgentBase* create_agent_breakout() {
    return new AgentBreakout();
}

}  // namespace etai
