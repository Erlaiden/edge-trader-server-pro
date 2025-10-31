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
double get_fee_per_trade();   // ETAI_FEE_BPS/10000
double get_alpha_sharpe();    // α
double get_lambda_risk();     // базовая λ
double get_mu_manip();        // базовая μ
void   set_fee_per_trade(double v);
void   set_alpha_sharpe(double v);
void   set_lambda_risk(double v);
void   set_mu_manip(double v);
void   init_rewardv2_from_env();

// Anti-manip (validation telemetry)
double get_val_manip_ratio();     // нормализованный ratio
void   set_val_manip_ratio(double v);
double get_val_manip_flagged();   // count как double
void   set_val_manip_flagged(double v);

// Dynamic coeffs (effective values used in last train)
double get_lambda_risk_eff();
void   set_lambda_risk_eff(double v);
double get_mu_manip_eff();
void   set_mu_manip_eff(double v);

// Tunables from ENV for dynamics
double get_sigma_ref();       // ETAI_SIGMA_REF, дефолт 0.01
double get_lambda_kvol();     // ETAI_LAMBDA_KVOL, дефолт 2.0
double get_mu_kfreq();        // ETAI_MU_KFREQ, дефолт 3.0

} // namespace etai
