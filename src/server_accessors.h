#pragma once
#include <atomic>

namespace etai {

// --- Model knobs ---
double get_model_thr();
long long get_model_ma_len();
int get_model_feat_dim();

// --- Last inference telemetry ---
double get_last_infer_score();
double get_last_infer_sigma();
int    get_last_infer_signal();

// --- Reward v2 telemetry (обновляется при обучении/валидации) ---
double get_reward_avg();
double get_reward_sharpe();
double get_reward_winrate();
double get_reward_drawdown();

void set_reward_avg(double v);
void set_reward_sharpe(double v);
void set_reward_winrate(double v);
void set_reward_drawdown(double v);

// --- Config for Reward v2: weights and fees (из ENV, но можно обновлять рантайм-методами) ---
double get_fee_per_trade();    // абсолютная доля на сделку, для простоты “bps/10000”
double get_alpha_sharpe();     // α
double get_lambda_risk();      // λ
double get_mu_manip();         // μ

void set_fee_per_trade(double v);
void set_alpha_sharpe(double v);
void set_lambda_risk(double v);
void set_mu_manip(double v);

// init из ENV — вызывать при старте
void init_rewardv2_from_env();

}
