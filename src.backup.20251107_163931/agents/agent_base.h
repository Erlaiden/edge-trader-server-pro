#pragma once
#include <armadillo>
#include <string>
#include <memory>

// Абстрактный базовый класс для всех торговых агентов PRO-X.
// Каждый агент реализует стратегию принятия решения на основе признаков PPO-PRO.
// Интерфейс унифицирован под модель логрег (thr, features).
namespace etai {

class AgentBase {
public:
    virtual ~AgentBase() = default;

    // Основное решение: вернуть +1 (long), 0 (flat), −1 (short)
    virtual int decide(const arma::rowvec& features, double thr) = 0;

    // Имя агента
    virtual std::string name() const = 0;

    // Вспомогательные метрики
    virtual double confidence() const { return last_confidence; }
    virtual void reset() { last_confidence = 0.0; }

protected:
    double last_confidence = 0.0;  // степень уверенности агента
};

// Умный указатель для хранения и фабричных вызовов
using AgentPtr = std::shared_ptr<AgentBase>;

}  // namespace etai
