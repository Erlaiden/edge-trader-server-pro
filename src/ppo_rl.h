#pragma once
#include <armadillo>
#include "json.hpp"

using json = nlohmann::json;
using namespace arma;

namespace etai {

// Forward declarations - структуры ВНЕ классов
struct ForwardCache {
    vec x, h1, h2, out;
    vec z1, z2;
};

struct NNGradients {
    mat dW1, dW2, dW3;
    vec db1, db2, db3;
    void zero();
};

// Простая нейросеть
class SimpleNN {
public:
    mat W1, W2, W3;
    vec b1, b2, b3;
    
    SimpleNN(int input_dim, int hidden1, int hidden2, int output_dim);
    vec forward(const vec& x);
    void init_weights();
    ForwardCache forward_cached(const vec& x);
    NNGradients backward(const ForwardCache& cache, const vec& d_out);
    void update_weights(const NNGradients& grads, double lr);
    json to_json() const;
    void from_json(const json& j);
};

// Actor
class Actor {
public:
    SimpleNN net;
    Actor(int state_dim, int hidden1=64, int hidden2=32);
    vec get_action_probs(const vec& state);
};

// Critic
class Critic {
public:
    SimpleNN net;
    Critic(int state_dim, int hidden1=64, int hidden2=32);
    double get_value(const vec& state);
};

// PPO Config
struct PPOConfig {
    double learning_rate = 3e-4;
    double gamma = 0.99;
    double lambda_gae = 0.95;
    double clip_epsilon = 0.2;
    double entropy_coef = 0.01;
    int epochs_per_update = 10;
    int batch_size = 64;
    int rollout_steps = 2048;
};

// Main training function
json trainPPO_RL(
    const mat& raw15,
    const mat* raw60,
    const mat* raw240,
    const mat* raw1440,
    int total_timesteps,
    double tp, double sl, int ma_len,
    const PPOConfig& config
);

} // namespace etai
