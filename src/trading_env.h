#pragma once
#include <armadillo>
#include <vector>

namespace etai {

struct TradeResult {
    double reward;
    bool done;
};

// Trading Environment для PPO
class TradingEnv {
private:
    arma::mat features;     // (N x D) матрица фич
    arma::vec future_returns; // (N) будущие доходности
    int current_step;
    int max_steps;
    double tp_threshold;
    double sl_threshold;
    double fee;
    
public:
    TradingEnv(const arma::mat& feat, const arma::vec& fut_ret, 
               double tp, double sl, double fee_pct=0.0004);
    
    arma::vec reset();  // сброс в начало
    TradeResult step(int action);  // 0=SHORT, 1=LONG
    arma::vec get_state();
    bool is_done();
};

} // namespace etai
