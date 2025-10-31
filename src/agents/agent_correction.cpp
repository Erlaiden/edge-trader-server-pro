#include "agent_base.h"
#include <armadillo>
#include <cmath>

namespace etai {

// Агент Correction: контртрендовая логика.
// Если сигнал экстремален (очень высокое |feature|), открывает противоположную позицию.
class AgentCorrection : public AgentBase {
public:
    AgentCorrection() = default;
    ~AgentCorrection() override = default;

    int decide(const arma::rowvec& features, double thr) override {
        double skew = arma::mean(features);
        double magnitude = arma::mean(arma::abs(features));

        // Уверенность выше, если рынок "перегрет" (высокая амплитуда)
        last_confidence = std::tanh(magnitude);

        if (last_confidence > thr) {
            // Если рынок перегрет вверх — открываем short, и наоборот
            return (skew > 0) ? -1 : +1;
        }
        return 0;
    }

    std::string name() const override {
        return "agent_correction";
    }
};

// Фабричная функция
extern "C" AgentBase* create_agent_correction() {
    return new AgentCorrection();
}

}  // namespace etai
