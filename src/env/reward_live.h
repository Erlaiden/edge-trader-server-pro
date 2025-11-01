#pragma once

namespace etai {

struct RewardParams {
  double risk_lambda = 1.0; // штраф за риск
  double dd_penalty  = 1.0; // вес за drawdown
  double pf_gain     = 1.0; // вес за profit factor
};

// Объявление; реализация в reward_live.cpp
double reward_live(double profit_t, double dd_t, double win_smooth_t,
                   const RewardParams& rp);

} // namespace etai
