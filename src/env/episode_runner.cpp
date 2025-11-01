#include "env/episode_runner.h"
#include <algorithm>

namespace etai {

Trajectory EpisodeRunner::run_fixed(EnvTrading& env,
                                    std::function<int(const std::vector<double>&)> policy,
                                    std::size_t max_len)
{
  Trajectory tr{};
  auto state = env.reset();
  if (state.empty()) return tr;

  for (std::size_t i = 0; i < max_len; ++i) {
    int act = 0;
    try { act = policy(state); } catch(...) { act = 0; }
    auto io = env.step(act);
    tr.rewards.push_back(io.reward);
    if (io.reward > 0) ++tr.wins;
    else if (io.reward < 0) ++tr.losses;
    state = io.next_state;
    if (io.done) { tr.steps = i + 1; break; }
  }
  if (tr.steps == 0) tr.steps = std::min<std::size_t>(max_len, tr.rewards.size());
  tr.equity_final = env.equity();
  tr.max_dd = env.max_dd();
  return tr;
}

} // namespace etai
