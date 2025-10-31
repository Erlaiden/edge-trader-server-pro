#include "train_logic.h"
#include "server_accessors.h"   // etai::{set_model_thr,set_model_ma_len,set_model_feat_dim,set_current_model}
#include "ppo_pro.h"
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

    // --- 1) Базовый TF (15) или указанный interval
    arma::mat raw15;
    if (!load_raw_ohlcv(symbol, interval, raw15))
        throw std::runtime_error("Failed to load OHLCV");

    // --- 2) Опциональные HTF (60/240/1440) — безопасная подгрузка
    arma::mat raw60, raw240, raw1440;
    const arma::mat *p60   = nullptr;
    const arma::mat *p240  = nullptr;
    const arma::mat *p1440 = nullptr;

    if (try_load_raw(symbol, "60", raw60))     p60   = &raw60;
    if (try_load_raw(symbol, "240", raw240))   p240  = &raw240;
    if (try_load_raw(symbol, "1440", raw1440)) p1440 = &raw1440;

    std::cout << "[TRAIN] shapes: 15=" << raw15.n_rows
              << " 60="   << (p60   ? raw60.n_rows   : 0)
              << " 240="  << (p240  ? raw240.n_rows  : 0)
              << " 1440=" << (p1440 ? raw1440.n_rows : 0)
              << "  (cols15=" << raw15.n_cols << ")\n";

    // --- 3) Тренировка (HTF передаём указателями; внутри могут не использоваться — это ок)
    json trainer = trainPPO_pro(raw15, p60, p240, p1440, episodes, tp, sl, ma_len, use_antimanip);

    // --- 4) Сохранение модели на диск
    const std::string model_path = "cache/models/" + symbol + "_" + interval + "_ppo_pro.json";
    std::ofstream(model_path) << trainer.dump(2);

    // --- 5) Обновляем атомики для health/metrics (thr, ma, feat_dim, current_model)
    // best_thr
    const double best_thr = trainer.value("best_thr", 0.5);

    // feat_dim: сначала пытаемся из policy.feat_dim, иначе из metrics.feat_cols
    int feat_dim = 0;
    try {
        if (trainer.contains("policy") &&
            trainer["policy"].contains("feat_dim") &&
            !trainer["policy"]["feat_dim"].is_null()) {
            feat_dim = trainer["policy"]["feat_dim"].get<int>();
        }
    } catch (...) {}

    if (feat_dim <= 0) {
        try {
            if (trainer.contains("metrics") &&
                trainer["metrics"].contains("feat_cols") &&
                !trainer["metrics"]["feat_cols"].is_null()) {
                feat_dim = trainer["metrics"]["feat_cols"].get<int>();
            }
        } catch (...) {}
    }

    set_model_thr(best_thr);
    set_model_ma_len(ma_len);
    if (feat_dim > 0) set_model_feat_dim(feat_dim);
    set_current_model(trainer);

    // --- 6) Лог в stdout для верификации
    try {
        const auto& m = trainer.at("metrics");
        std::cout << "[TRAIN] rows="       << m.value("N_rows", 0)
                  << " M_labeled="         << m.value("M_labeled", 0)
                  << " best_thr="          << best_thr
                  << " acc="               << m.value("val_accuracy", 0.0)
                  << " Rv1="               << m.value("val_reward_v1", m.value("val_reward", 0.0))
                  << " profit="            << m.value("val_profit_avg", 0.0)
                  << " sharpe="            << m.value("val_sharpe", 0.0)
                  << " winrate="           << m.value("val_winrate", 0.0)
                  << " dd="                << m.value("val_drawdown", 0.0)
                  << " Rv2="               << m.value("val_reward_v2", 0.0)
                  << " feat_dim="          << feat_dim
                  << std::endl;
    } catch (...) {
        std::cout << "[TRAIN] metrics missing (feat_dim=" << feat_dim << ")\n";
    }

    // --- 7) Стабилизированный ответ клиенту
    return make_train_reply(trainer, tp, sl, ma_len, model_path);
}

} // namespace etai
