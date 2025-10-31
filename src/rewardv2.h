#pragma once
#include <cstddef>

namespace etai { namespace rv2 {

// telemetry getters
double get_reward_avg();
double get_reward_sharpe();
double get_reward_winrate();
double get_reward_drawdown();

// telemetry setters
void set_reward_avg(double v);
void set_reward_sharpe(double v);
void set_reward_winrate(double v);
void set_reward_drawdown(double v);

// config (α, λ, μ, fee)
double get_fee_per_trade();
double get_alpha_sharpe();
double get_lambda_risk();
double get_mu_manip();

void set_fee_per_trade(double v);
void set_alpha_sharpe(double v);
void set_lambda_risk(double v);
void set_mu_manip(double v);

// env init (безопасен к многократному вызову)
void init_from_env();

}} // namespace etai::rv2
