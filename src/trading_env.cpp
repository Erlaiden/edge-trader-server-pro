#include "trading_env.h"

namespace etai {

TradingEnv::TradingEnv(const arma::mat& feat, const arma::vec& fut_ret,
                       double tp, double sl, double fee_pct)
    : features(feat), future_returns(fut_ret),
      current_step(0), max_steps(feat.n_rows - 1),
      tp_threshold(tp), sl_threshold(sl), fee(fee_pct) {}

arma::vec TradingEnv::reset() {
    current_step = 0;
    return get_state();
}

arma::vec TradingEnv::get_state() {
    if (current_step >= max_steps) {
        return arma::vec(features.n_cols, arma::fill::zeros);
    }
    return features.row(current_step).t();
}

bool TradingEnv::is_done() {
    return current_step >= max_steps;
}

TradeResult TradingEnv::step(int action) {
    TradeResult result;
    result.done = false;
    result.reward = 0.0;
    
    if (current_step >= max_steps) {
        result.done = true;
        return result;
    }
    
    double fut_ret = future_returns(current_step);
    
    // action: 0=SHORT, 1=LONG
    if (action == 1) {  // LONG
        if (fut_ret >= tp_threshold) {
            result.reward = tp_threshold - fee;  // TP hit
        } else if (fut_ret <= -sl_threshold) {
            result.reward = -sl_threshold - fee;  // SL hit
        } else {
            result.reward = fut_ret - fee;  // Close at market
        }
    } else {  // SHORT (action == 0)
        if (fut_ret <= -sl_threshold) {
            result.reward = tp_threshold - fee;  // TP hit (inverse)
        } else if (fut_ret >= tp_threshold) {
            result.reward = -sl_threshold - fee;  // SL hit (inverse)
        } else {
            result.reward = -fut_ret - fee;  // Close at market (inverse)
        }
    }
    
    current_step++;
    result.done = (current_step >= max_steps);
    
    return result;
}

} // namespace etai
