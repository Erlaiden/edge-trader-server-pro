#pragma once

namespace etai {

// Телеметрия Reward v2
double get_reward_avg();
double get_reward_sharpe();
double get_reward_winrate();
double get_reward_drawdown();
void   set_reward_avg(double v);
void   set_reward_sharpe(double v);
void   set_reward_winrate(double v);
void   set_reward_drawdown(double v);

// Конфиги Reward v2 (иниц. из ENV)
double get_fee_per_trade();    // ETAI_FEE_BPS/10000
double get_alpha_sharpe();     // α
double get_lambda_risk();      // λ
double get_mu_manip();         // μ
void   set_fee_per_trade(double v);
void   set_alpha_sharpe(double v);
void   set_lambda_risk(double v);
void   set_mu_manip(double v);
void   init_rewardv2_from_env();

} // namespace etai
