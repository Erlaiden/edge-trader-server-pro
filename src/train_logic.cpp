#include "train_logic.h"
#include "server_accessors.h"
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

json run_train_pro_and_save(const std::string& symbol,
                            const std::string& interval,
                            int episodes,
                            double tp,
                            double sl,
                            int ma_len,
                            bool use_antimanip)
{
    std::lock_guard<std::mutex> lock(train_mutex);

    arma::mat raw;
    if (!load_raw_ohlcv(symbol, interval, raw))
        throw std::runtime_error("Failed to load OHLCV");

    // 1) тренировка → JSON результата тренера
    json trainer = trainPPO_pro(raw, nullptr, nullptr, nullptr, episodes, tp, sl, ma_len, use_antimanip);

    // 2) путь модели и сохранение
    const std::string model_path = "cache/models/" + symbol + "_" + interval + "_ppo_pro.json";
    std::ofstream(model_path) << trainer.dump(2);

    // 3) обновляем атомики
    set_model_thr(trainer.value("best_thr", 0.5));
    set_model_ma_len(ma_len);
    set_current_model(trainer);

    // 4) лог для верификации
    try {
        const auto& m = trainer.at("metrics");
        std::cout << "[TRAIN] rows=" << m.value("N_rows", 0)
                  << " M_labeled=" << m.value("M_labeled", 0)
                  << " manip_rej=" << m.value("manip_rejected", 0)
                  << " best_thr=" << trainer.value("best_thr", 0.0)
                  << " acc=" << m.value("val_accuracy", 0.0)
                  << " rew=" << m.value("val_reward", 0.0) << std::endl;
    } catch (...) {
        std::cout << "[TRAIN] metrics missing\n";
    }

    // 5) стабилизированный ответ
    return make_train_reply(trainer, tp, sl, ma_len, model_path);
}

} // namespace etai
