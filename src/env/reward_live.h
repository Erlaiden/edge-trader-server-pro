#pragma once
#include <cmath>

/*
  RewardLive v1 — заглушка формулы вознаграждения.
  Без реализации. Параметры соответствуют RFC.
*/

namespace etai {

struct RewardParams {
  double risk_lambda = 1.0;  // штраф за риск
  double dd_penalty = 1.0;   // вес за drawdown
  double pf_gain = 1.0;      // вес за profit factor
};

inline double reward_live(double profit_t, double dd_t, double win_smooth_t,
                          const RewardParams& rp) {
  // Заглушка: вернём 0. Реализация появится на этапе B.
  (void)profit_t; (void)dd_t; (void)win_smooth_t; (void)rp;
  return 0.0;
}

} // namespace etai
