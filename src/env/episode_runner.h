#pragma once
#include <vector>
#include <functional>
#include <cstddef>

/*
  EpisodeRunner v1 — заглушка обвязки rollout:
  - собирает траекторию;
  - будет считать advantage/GAE на этапе B.
*/

namespace etai {

struct Trajectory {
  std::vector<std::vector<double>> states;
  std::vector<int> actions;
  std::vector<double> rewards;
  std::vector<double> values;     // для GAE
  std::vector<double> advantages; // позже
};

class EpisodeRunner {
public:
  EpisodeRunner() = default;

  template <typename Env, typename PolicyFn>
  Trajectory run(Env& env, PolicyFn policy, std::size_t max_len) {
    (void)env; (void)policy; (void)max_len;
    return {}; // заглушка
  }
};

} // namespace etai
