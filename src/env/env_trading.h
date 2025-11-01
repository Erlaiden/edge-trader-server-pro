#pragma once
#include <vector>
#include <cstddef>
#include <cstdint>

namespace etai {

struct EnvConfig {
  double start_equity = 1.0;
  double fee_per_trade = 0.0005; // комиссия за действие |action|
  double slippage = 0.0;
  int feat_dim = 32;
};

struct StepIO {
  std::vector<double> next_state; // размер feat_dim
  double reward = 0.0;            // за шаг
  bool done = false;
};

class EnvTrading {
public:
  EnvTrading() = default;

  // Инициализация из фич и цен закрытия
  void set_dataset(const std::vector<std::vector<double>>& feats,
                   const std::vector<double>& closes,
                   const EnvConfig& cfg);

  // Начало эпизода
  std::vector<double> reset(std::uint64_t seed = 0);

  // Шаг: action ∈ {-1,0,+1}
  StepIO step(int action);

  // Метрики
  double equity() const { return equity_; }
  double max_dd() const { return max_dd_; }
  std::size_t t() const { return t_; }
  const EnvConfig& config() const { return cfg_; }

private:
  EnvConfig cfg_{};
  std::vector<std::vector<double>> feats_;
  std::vector<double> closes_;
  std::size_t t_ = 0;
  double equity_ = 1.0;
  double peak_ = 1.0;
  double max_dd_ = 0.0;
};

} // namespace etai
