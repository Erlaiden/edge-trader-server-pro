#pragma once
#include <vector>
#include <functional>
#include <cstddef>
#include "env/env_trading.h"

namespace etai {

struct Trajectory {
  std::vector<double> rewards;
  std::size_t steps = 0;
  double equity_final = 0.0;
  double max_dd = 0.0;
  int wins = 0;
  int losses = 0;
};

class EpisodeRunner {
public:
  EpisodeRunner() = default;

  // policy(state)->action {-1,0,1}
  Trajectory run_fixed(EnvTrading& env,
                       std::function<int(const std::vector<double>&)> policy,
                       std::size_t max_len);
};

} // namespace etai
