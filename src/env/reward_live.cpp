#include "env/reward_live.h"

namespace etai {

double reward_live(double /*profit_t*/, double /*dd_t*/, double /*win_smooth_t*/,
                   const RewardParams& /*rp*/) {
  return 0.0; // заглушка
}

} // namespace etai
