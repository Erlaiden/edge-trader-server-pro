#include "ppo_rl.h"
#include "trading_env.h"
#include <cmath>

namespace etai {

// ============================================================================
// SimpleNN Implementation
// ============================================================================

SimpleNN::SimpleNN(int input_dim, int hidden1, int hidden2, int output_dim) {
    W1.set_size(hidden1, input_dim);
    b1.set_size(hidden1);
    W2.set_size(hidden2, hidden1);
    b2.set_size(hidden2);
    W3.set_size(output_dim, hidden2);
    b3.set_size(output_dim);
    init_weights();
}

void SimpleNN::init_weights() {
    W1.randn(); W1 *= std::sqrt(2.0 / W1.n_cols);
    W2.randn(); W2 *= std::sqrt(2.0 / W2.n_cols);
    W3.randn(); W3 *= std::sqrt(2.0 / W3.n_cols);
    b1.zeros();
    b2.zeros();
    b3.zeros();
}

arma::vec SimpleNN::forward(const arma::vec& x) {
    arma::vec h1 = W1 * x + b1;
    h1 = arma::clamp(h1, 0.0, 1e10);
    arma::vec h2 = W2 * h1 + b2;
    h2 = arma::clamp(h2, 0.0, 1e10);
    arma::vec out = W3 * h2 + b3;
    return out;
}

ForwardCache SimpleNN::forward_cached(const arma::vec& x) {
    ForwardCache cache;
    cache.x = x;
    cache.z1 = W1 * x + b1;
    cache.h1 = arma::clamp(cache.z1, 0.0, 1e10);
    cache.z2 = W2 * cache.h1 + b2;
    cache.h2 = arma::clamp(cache.z2, 0.0, 1e10);
    cache.out = W3 * cache.h2 + b3;
    return cache;
}

NNGradients SimpleNN::backward(const ForwardCache& cache, const arma::vec& d_out) {
    NNGradients grads;
    grads.dW1.set_size(arma::size(W1));
    grads.dW2.set_size(arma::size(W2));
    grads.dW3.set_size(arma::size(W3));
    grads.db1.set_size(arma::size(b1));
    grads.db2.set_size(arma::size(b2));
    grads.db3.set_size(arma::size(b3));
    
    grads.dW3 = d_out * cache.h2.t();
    grads.db3 = d_out;
    
    arma::vec d_h2 = W3.t() * d_out;
    arma::vec d_z2 = d_h2 % (cache.z2 > 0);
    grads.dW2 = d_z2 * cache.h1.t();
    grads.db2 = d_z2;
    
    arma::vec d_h1 = W2.t() * d_z2;
    arma::vec d_z1 = d_h1 % (cache.z1 > 0);
    grads.dW1 = d_z1 * cache.x.t();
    grads.db1 = d_z1;
    
    return grads;
}

void SimpleNN::update_weights(const NNGradients& grads, double lr) {
    W1 -= lr * grads.dW1;
    b1 -= lr * grads.db1;
    W2 -= lr * grads.dW2;
    b2 -= lr * grads.db2;
    W3 -= lr * grads.dW3;
    b3 -= lr * grads.db3;
}

// ============================================================================
// Actor Implementation
// ============================================================================

Actor::Actor(int state_dim, int hidden1, int hidden2) 
    : net(state_dim, hidden1, hidden2, 2) {}

arma::vec Actor::get_action_probs(const arma::vec& state) {
    arma::vec logits = net.forward(state);
    arma::vec exp_logits = arma::exp(logits - arma::max(logits));
    return exp_logits / arma::accu(exp_logits);
}

// ============================================================================
// Critic Implementation
// ============================================================================

Critic::Critic(int state_dim, int hidden1, int hidden2)
    : net(state_dim, hidden1, hidden2, 1) {}

double Critic::get_value(const arma::vec& state) {
    arma::vec v = net.forward(state);
    return v(0);
}

// ============================================================================
// Helper: compute GAE
// ============================================================================

arma::vec compute_gae(const arma::vec& rewards, const arma::vec& values, 
                      const arma::vec& dones, double gamma, double lambda) {
    int n = rewards.n_elem;
    arma::vec advantages(n, arma::fill::zeros);
    double gae = 0.0;
    
    for (int t = n - 1; t >= 0; t--) {
        double delta = rewards(t) - values(t);
        if (t + 1 < n && dones(t) == 0) {
            delta += gamma * values(t + 1);
        }
        gae = delta + gamma * lambda * (dones(t) == 0 ? gae : 0.0);
        advantages(t) = gae;
    }
    return advantages;
}

// ============================================================================
// Helper: collect rollout
// ============================================================================

struct Rollout {
    arma::mat states;
    arma::ivec actions;
    arma::vec rewards;
    arma::vec values;
    arma::vec log_probs;
    arma::vec dones;
};

Rollout collect_rollout(TradingEnv& env, Actor& actor, Critic& critic, int rollout_steps) {
    Rollout rollout;
    std::vector<arma::vec> states_vec;
    std::vector<int> actions_vec;
    std::vector<double> rewards_vec, values_vec, log_probs_vec, dones_vec;
    
    arma::vec state = env.reset();
    
    for (int step = 0; step < rollout_steps && !env.is_done(); step++) {
        states_vec.push_back(state);
        
        arma::vec probs = actor.get_action_probs(state);
        arma::uword action_idx;
        probs.max(action_idx);
        int action = (int)action_idx;
        
        double log_prob = std::log(probs(action) + 1e-10);
        double value = critic.get_value(state);
        
        actions_vec.push_back(action);
        values_vec.push_back(value);
        log_probs_vec.push_back(log_prob);
        
        TradeResult result = env.step(action);
        rewards_vec.push_back(result.reward);
        dones_vec.push_back(result.done ? 1.0 : 0.0);
        
        if (!result.done) {
            state = env.get_state();
        } else {
            break;
        }
    }
    
    int n = states_vec.size();
    rollout.states.set_size(n, states_vec[0].n_elem);
    rollout.actions.set_size(n);
    rollout.rewards.set_size(n);
    rollout.values.set_size(n);
    rollout.log_probs.set_size(n);
    rollout.dones.set_size(n);
    
    for (int i = 0; i < n; i++) {
        rollout.states.row(i) = states_vec[i].t();
        rollout.actions(i) = actions_vec[i];
        rollout.rewards(i) = rewards_vec[i];
        rollout.values(i) = values_vec[i];
        rollout.log_probs(i) = log_probs_vec[i];
        rollout.dones(i) = dones_vec[i];
    }
    
    return rollout;
}

// ============================================================================
// PPO Training (stub - will be completed)
// ============================================================================

json trainPPO_RL(
    const arma::mat& raw15,
    const arma::mat* raw60,
    const arma::mat* raw240,
    const arma::mat* raw1440,
    int total_timesteps,
    double tp, double sl, int ma_len,
    const PPOConfig& config) {
    
    json out = json::object();
    
    std::cout << "[PPO_RL] Training with total_timesteps=" << total_timesteps << "\n";
    std::cout << "[PPO_RL] Config: lr=" << config.learning_rate 
              << " gamma=" << config.gamma 
              << " clip=" << config.clip_epsilon << "\n";
    
    out["ok"] = true;
    out["algorithm"] = "PPO_RL";
    out["total_timesteps"] = total_timesteps;
    out["message"] = "PPO with gradients - ready for training loop";
    out["best_thr"] = 0.5;
    out["metrics"] = json::object();
    out["metrics"]["val_accuracy"] = 0.0;
    out["metrics"]["val_reward"] = 0.0;
    
    return out;
}

} // namespace etai
