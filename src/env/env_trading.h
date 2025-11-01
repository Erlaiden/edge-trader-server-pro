#pragma once
#include <vector>
#include <cstdint>
#include <optional>
#include <string>

/*
  EnvTrading v1 — заглушка интерфейса (без реализации).
  Назначение: онлайновая среда для on-policy обучения.
  Не подключена к CMake. Безопасно для прод-ядра.
*/

namespace etai {

struct EnvConfig {
  double start_equity = 1.0;
  double fee_per_trade = 0.0005;
  double slippage = 0.0;
  double risk_lambda = 1.0;
  double max_drawdown = 0.25;
  int ma_len = 12;
  int feat_dim = 32;
};

struct StepIO {
  // Вход на шаг: action \in {-1,0,+1}
  int action = 0;
  // Выход:
  std::vector<double> next_state;  // размер feat_dim (или feat_dim+k)
  double reward = 0.0;
  bool done = false;
};

class EnvTrading {
public:
  EnvTrading() = default;
  explicit EnvTrading(const EnvConfig& cfg): cfg_(cfg) {}

  // Подготовка данных эпизода (история фич)
  void set_dataset(const std::vector<std::vector<double>>& feats);

  // Начало эпизода. Возвращает начальное состояние.
  std::vector<double> reset(std::uint64_t seed = 0);

  // Один шаг среды.
  StepIO step(int action);

  // Текущие показатели
  double equity() const { return equity_; }
  double max_dd() const { return max_dd_; }
  std::size_t t() const { return t_; }

  // Метаданные
  const EnvConfig& config() const { return cfg_; }

private:
  EnvConfig cfg_;
  std::vector<std::vector<double>> feats_;
  std::size_t t_ = 0;
  double equity_ = 1.0;
  double peak_equity_ = 1.0;
  double max_dd_ = 0.0;
};

} // namespace etai
