#include "ppo_rl.h"
#include "trading_env.h"
#include "features/features.h"
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

json SimpleNN::to_json() const {
    json j;
    j["W1"] = json::array();
    j["W2"] = json::array();
    j["W3"] = json::array();
    j["b1"] = json::array();
    j["b2"] = json::array();
    j["b3"] = json::array();
    
    for (arma::uword i = 0; i < W1.n_elem; i++) j["W1"].push_back(W1(i));
    for (arma::uword i = 0; i < W2.n_elem; i++) j["W2"].push_back(W2(i));
    for (arma::uword i = 0; i < W3.n_elem; i++) j["W3"].push_back(W3(i));
    for (arma::uword i = 0; i < b1.n_elem; i++) j["b1"].push_back(b1(i));
    for (arma::uword i = 0; i < b2.n_elem; i++) j["b2"].push_back(b2(i));
    for (arma::uword i = 0; i < b3.n_elem; i++) j["b3"].push_back(b3(i));
    
    j["W1_shape"] = json::array({(int)W1.n_rows, (int)W1.n_cols});
    j["W2_shape"] = json::array({(int)W2.n_rows, (int)W2.n_cols});
    j["W3_shape"] = json::array({(int)W3.n_rows, (int)W3.n_cols});
    
    return j;
}

void SimpleNN::from_json(const json& j) {
    auto w1_shape = j["W1_shape"];
    auto w2_shape = j["W2_shape"];
    auto w3_shape = j["W3_shape"];
    
    W1.set_size(w1_shape[0].get<int>(), w1_shape[1].get<int>());
    W2.set_size(w2_shape[0].get<int>(), w2_shape[1].get<int>());
    W3.set_size(w3_shape[0].get<int>(), w3_shape[1].get<int>());
    b1.set_size(w1_shape[0].get<int>());
    b2.set_size(w2_shape[0].get<int>());
    b3.set_size(w3_shape[0].get<int>());
    
    for (arma::uword i = 0; i < W1.n_elem; i++) W1(i) = j["W1"][i].get<double>();
    for (arma::uword i = 0; i < W2.n_elem; i++) W2(i) = j["W2"][i].get<double>();
    for (arma::uword i = 0; i < W3.n_elem; i++) W3(i) = j["W3"][i].get<double>();
    for (arma::uword i = 0; i < b1.n_elem; i++) b1(i) = j["b1"][i].get<double>();
    for (arma::uword i = 0; i < b2.n_elem; i++) b2(i) = j["b2"][i].get<double>();
    for (arma::uword i = 0; i < b3.n_elem; i++) b3(i) = j["b3"][i].get<double>();
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
// PPO Update - Actor and Critic
// ============================================================================

void update_ppo(Actor& actor, Critic& critic, const Rollout& rollout,
                const arma::vec& advantages, const arma::vec& returns,
                const PPOConfig& config) {
    int n = rollout.states.n_rows;
    for (int epoch = 0; epoch < config.epochs_per_update; epoch++) {
        double total_actor_loss = 0.0, total_critic_loss = 0.0;
        for (int i = 0; i < n; i++) {
            arma::vec state = rollout.states.row(i).t();
            int action = rollout.actions(i);
            double old_log_prob = rollout.log_probs(i);
            double advantage = advantages(i);
            double return_val = returns(i);
            arma::vec probs = actor.get_action_probs(state);
            double ratio = std::exp(std::log(probs(action) + 1e-10) - old_log_prob);
            double actor_loss = -std::min(ratio * advantage, std::clamp(ratio, 1.0 - config.clip_epsilon, 1.0 + config.clip_epsilon) * advantage);
            arma::vec d_logits(probs.n_elem, arma::fill::zeros);
            d_logits(action) = -advantage * ratio;
            ForwardCache cache = actor.net.forward_cached(state);
            NNGradients grads = actor.net.backward(cache, d_logits);
            actor.net.update_weights(grads, config.learning_rate);
            double value_pred = critic.get_value(state);
            double critic_loss = std::pow(return_val - value_pred, 2);
            total_critic_loss += critic_loss;
            arma::vec d_value(1); d_value(0) = -2.0 * (return_val - value_pred);
            ForwardCache critic_cache = critic.net.forward_cached(state);
            NNGradients critic_grads = critic.net.backward(critic_cache, d_value);
            critic.net.update_weights(critic_grads, config.learning_rate);
        }
        if (epoch % 2 == 0) std::cout << "[PPO] Epoch " << epoch << " | Actor: " << (total_actor_loss / n) << " | Critic: " << (total_critic_loss / n) << "\n";
    }
}
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
    std::cout << "[PPO_RL] Starting training: " << total_timesteps << " timesteps\n";
    
    // Build real feature matrix from OHLCV data
    if (raw15.n_rows < 300 || raw15.n_cols < 6) {
        out["ok"] = false;
        out["error"] = "insufficient_data";
        return out;
    }

    arma::mat F = build_feature_matrix(raw15);
    const arma::uword N = F.n_rows;
    const arma::uword D = F.n_cols;
    
    std::cout << "[PPO_RL] Feature matrix: " << N << " rows x " << D << " cols\n";

    // Compute future returns
    arma::vec close = raw15.col(4);
    arma::vec r(N, arma::fill::zeros);
    for (arma::uword i = 0; i + 1 < N; ++i) {
        double c0 = close(i), c1 = close(i + 1);
        r(i) = (c0 > 0.0) ? (c1 / c0 - 1.0) : 0.0;
    }
    arma::vec future_returns = arma::shift(r, -1);
    future_returns(N - 1) = 0.0;

    // Filter by TP/SL thresholds
    double thr_pos = std::max(1e-4, std::min(tp, 1e-1));
    double thr_neg = std::max(1e-4, std::min(sl, 1e-1));
    
    std::vector<arma::uword> idx;
    idx.reserve(N);
    for (arma::uword i = 0; i < N; ++i) {
        double fr = future_returns(i);
        if (fr >= thr_pos || fr <= -thr_neg) {
            idx.push_back(i);
        }
    }

    const arma::uword M = idx.size();
    std::cout << "[PPO_RL] Filtered " << M << " samples from " << N << " (TP=" << thr_pos << ", SL=" << thr_neg << ")\n";
    
    if (M < 200) {
        out["ok"] = false;
        out["error"] = "not_enough_labeled";
        out["M_labeled"] = (int)M;
        out["N_rows"] = (int)N;
        return out;
    }

    // Use filtered samples
    arma::mat features(M, D, arma::fill::zeros);
    arma::vec filtered_returns(M, arma::fill::zeros);
    for (arma::uword k = 0; k < M; ++k) {
        arma::uword i = idx[k];
        features.row(k) = F.row(i);
        filtered_returns(k) = future_returns(i);
    }
    
    future_returns = filtered_returns;
    
    // Normalize
    arma::vec mu = arma::mean(features, 0).t();
    arma::vec sd = arma::stddev(features, 0, 0).t();
    for (arma::uword j = 0; j < features.n_cols; j++) {
        double s = (std::isfinite(sd(j)) && sd(j) > 1e-12) ? sd(j) : 1.0;
        features.col(j) = (features.col(j) - mu(j)) / s;
    }
    
    TradingEnv env(features, future_returns, tp, sl);
    Actor actor(features.n_cols, 64, 32);
    Critic critic(features.n_cols, 64, 32);
    
    int num_updates = total_timesteps / config.rollout_steps;
    double best_reward = -1e100;
    
    for (int update = 0; update < num_updates; update++) {
        Rollout rollout = collect_rollout(env, actor, critic, config.rollout_steps);
        arma::vec advantages = compute_gae(rollout.rewards, rollout.values, rollout.dones, config.gamma, config.lambda_gae);
        arma::vec returns = advantages + rollout.values;
        double adv_mean = arma::mean(advantages);
        double adv_std = arma::stddev(advantages);
        if (adv_std > 1e-8) advantages = (advantages - adv_mean) / (adv_std + 1e-8);
        update_ppo(actor, critic, rollout, advantages, returns, config);
        double avg_reward = arma::mean(rollout.rewards);
        if (avg_reward > best_reward) best_reward = avg_reward;
        if (update % 10 == 0) std::cout << "[PPO_RL] Update " << update << "/" << num_updates << " | Reward: " << avg_reward << "\n";
    }
    
    out["ok"] = true;
    out["algorithm"] = "PPO_RL";
    out["best_thr"] = 0.5;
    
    // Critical fields for model validation
    out["version"] = 10;  // FEAT_VERSION (32 features with MFLOW)
    out["feat_dim"] = (int)D;
    out["tp"] = tp;
    out["sl"] = sl;
    
    // Training results
    out["metrics"] = json::object();
    out["metrics"]["best_reward"] = best_reward;
    out["metrics"]["M_labeled"] = (int)M;
    out["metrics"]["N_rows"] = (int)N;
    out["metrics"]["version"] = 10;
    out["metrics"]["feat_dim"] = (int)D;
    out["metrics"]["tp"] = tp;
    out["metrics"]["sl"] = sl;
    // Save Actor and Critic weights
    out["actor_weights"] = actor.net.to_json();
    out["critic_weights"] = critic.net.to_json();
    
    std::cout << "[PPO_RL] Weights saved to output JSON\n";
    
    return out;
}

} // namespace etai
