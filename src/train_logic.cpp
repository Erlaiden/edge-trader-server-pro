#include "train_logic.h"
#include "server_accessors.h"
#include "ppo_pro.h"
#include "ppo_rl.h"
#include "utils_data.h"
#include "http_reply.h"
#include <armadillo>
#include <mutex>
#include <iostream>
#include <fstream>

using json = nlohmann::json;

namespace etai {
static std::mutex train_mutex;

static inline bool try_load_raw(const std::string& symbol,
                                const std::string& interval,
                                arma::mat& out)
{
    if (!load_raw_ohlcv(symbol, interval, out)) {
        std::cerr << "[TRAIN] warn: HTF " << interval << " not loaded\n";
        out.reset();
        return false;
    }
    if (out.n_cols < 6 || out.n_rows < 30) {
        std::cerr << "[TRAIN] warn: HTF " << interval
                  << " bad shape rows=" << out.n_rows << " cols=" << out.n_cols << "\n";
        out.reset();
        return false;
    }
    return true;
}

json run_train_pro_and_save(const std::string& symbol,
                            const std::string& interval,
                            int episodes,
                            double tp,
                            double sl,
                            int ma_len,
                            bool use_antimanip)
{
    std::lock_guard<std::mutex> lock(train_mutex);

    // --- 1) Загрузка данных
    arma::mat raw15;
    if (!load_raw_ohlcv(symbol, interval, raw15))
        throw std::runtime_error("Failed to load OHLCV");

    arma::mat raw60, raw240, raw1440;
    const arma::mat *p60   = nullptr;
    const arma::mat *p240  = nullptr;
    const arma::mat *p1440 = nullptr;

    if (try_load_raw(symbol, "60",   raw60))   p60   = &raw60;
    if (try_load_raw(symbol, "240",  raw240))  p240  = &raw240;
    if (try_load_raw(symbol, "1440", raw1440)) p1440 = &raw1440;

    std::cout << "[TRAIN] shapes: 15=" << raw15.n_rows
              << " 60="   << (p60   ? raw60.n_rows   : 0)
              << " 240="  << (p240  ? raw240.n_rows  : 0)
              << " 1440=" << (p1440 ? raw1440.n_rows : 0)
              << "  (cols15=" << raw15.n_cols << ")\n";

    // --- 2) Обучение модели (теперь ppo_pro.cpp добавляет tp/sl/feat_dim/version в metrics)
    // --- 2) Обучение модели с PPO Reinforcement Learning
    PPOConfig config;  // Используем дефолтные параметры
    episodes = std::min(episodes, 100);  // Max 1000 episodes = 50 min
    int total_timesteps = episodes * 2048;  // episodes -> timesteps (rollout_steps=2048)
    json trainer = trainPPO_RL(raw15, p60, p240, p1440, total_timesteps, tp, sl, ma_len, config);

    // --- 3) Добавляем служебные поля на верхний уровень
    trainer["symbol"]   = symbol;
    trainer["interval"] = interval;
    trainer["ma_len"]   = ma_len;
    
    // FIXED: Копируем критические поля из metrics если их ещё нет на top-level
    if (trainer.contains("metrics")) {
        const auto& m = trainer["metrics"];
        if (m.contains("tp") && !trainer.contains("tp"))
            trainer["tp"] = m["tp"];
        if (m.contains("sl") && !trainer.contains("sl"))
            trainer["sl"] = m["sl"];
        if (m.contains("version") && !trainer.contains("version"))
            trainer["version"] = m["version"];
        if (m.contains("feat_dim") && !trainer.contains("feat_dim"))
            trainer["feat_dim"] = m["feat_dim"];
    }

    // --- 4) Валидация что критические поля присутствуют
    bool model_valid = true;
    std::string validation_error;
    
    if (!trainer.contains("version") || trainer["version"].is_null()) {
        model_valid = false;
        validation_error += "version missing; ";
    }
    if (!trainer.contains("feat_dim") || trainer["feat_dim"].is_null() || trainer["feat_dim"].get<int>() <= 0) {
        model_valid = false;
        validation_error += "feat_dim invalid; ";
    }
    if (!trainer.contains("tp") || trainer["tp"].is_null()) {
        model_valid = false;
        validation_error += "tp missing; ";
    }
    if (!trainer.contains("sl") || trainer["sl"].is_null()) {
        model_valid = false;
        validation_error += "sl missing; ";
    }
    if (!trainer.contains("metrics") || !trainer["metrics"].contains("M_labeled") || 
        trainer["metrics"]["M_labeled"].get<int>() < 200) {
        model_valid = false;
        validation_error += "insufficient training data; ";
    }

    trainer["model_valid"] = model_valid;
    if (!model_valid) {
        trainer["validation_error"] = validation_error;
        std::cerr << "[TRAIN] WARNING: Model validation failed: " << validation_error << "\n";
    }

    // --- 5) Сохранение модели на диск (только если валидна)
    const std::string model_path = "cache/models/" + symbol + "_" + interval + "_ppo_pro.json";
    {
        std::ofstream f(model_path);
        if (!f) {
            throw std::runtime_error("Failed to open model file for writing: " + model_path);
        }
        f << trainer.dump(2);
    }
    std::cout << "[TRAIN] Model saved to " << model_path << " (valid=" << model_valid << ")\n";

    // --- 6) Обновляем атомики для health/metrics (только если валидна)
    if (model_valid) {
        const double best_thr = trainer.value("best_thr", 0.5);
        const int feat_dim = trainer.value("feat_dim", 0);
        
        set_model_thr(best_thr);
        set_model_ma_len(ma_len);
        if (feat_dim > 0) set_model_feat_dim(feat_dim);
        set_current_model(trainer);
    }

    // --- 7) Логирование
    try {
        const auto& m = trainer.at("metrics");
        std::cout << "[TRAIN] rows="       << m.value("N_rows", 0)
                  << " M_labeled="         << m.value("M_labeled", 0)
                  << " best_thr="          << trainer.value("best_thr", 0.0)
                  << " acc="               << m.value("val_accuracy", 0.0)
                  << " Rv1="               << m.value("val_reward_v1", m.value("val_reward", 0.0))
                  << " profit="            << m.value("val_profit_avg", 0.0)
                  << " sharpe="            << m.value("val_sharpe", 0.0)
                  << " winrate="           << m.value("val_winrate", 0.0)
                  << " dd="                << m.value("val_drawdown", 0.0)
                  << " Rv2="               << m.value("val_reward_v2", 0.0)
                  << " feat_dim="          << trainer.value("feat_dim", 0)
                  << " version="           << trainer.value("version", 0)
                  << " tp="                << trainer.value("tp", 0.0)
                  << " sl="                << trainer.value("sl", 0.0)
                  << " ma="                << ma_len
                  << " valid="             << model_valid
                  << std::endl;
    } catch (...) {
        std::cout << "[TRAIN] metrics missing\n";
    }

    // --- 8) Возврат результата
    return make_train_reply(trainer, tp, sl, ma_len, model_path);
}

} // namespace etai
