#include "train_logic.h"
#include "server_accessors.h"
#include "ppo_pro.h"
#include "utils_data.h"
#include "json.hpp"
#include <armadillo>
#include <mutex>
#include <iostream>
#include <fstream>

using json = nlohmann::json;

namespace etai {

static std::mutex train_mutex;

// безопасный доступ к числу
static double jget(const json& j, const std::string& k) {
    try {
        if (!j.contains(k)) return NAN;
        if (j[k].is_number()) return j[k].get<double>();
        if (j[k].is_string()) return std::stod(j[k].get<std::string>());
        return NAN;
    } catch (...) { return NAN; }
}

json run_train_pro_and_save(const std::string& symbol,
                            const std::string& interval,
                            int episodes,
                            double tp,
                            double sl,
                            int ma_len)
{
    std::lock_guard<std::mutex> lock(train_mutex);

    arma::mat raw;
    if (!load_raw_ohlcv(symbol, interval, raw))
        throw std::runtime_error("Failed to load OHLCV");

    json result = trainPPO_pro(raw, nullptr, nullptr, nullptr, episodes, tp, sl, ma_len);

    // ключевой момент — ссылка на вложенный объект
    json& metrics = result["metrics"];

    double best_thr   = jget(result,  "best_thr");
    double val_acc    = jget(metrics, "val_accuracy");
    double val_reward = jget(metrics, "val_reward");
    double M_labeled  = jget(metrics, "M_labeled");
    double val_size   = jget(metrics, "val_size");

    set_model_thr(best_thr);
    set_model_ma_len(ma_len);
    set_current_model(result);

    std::string model_path = "cache/models/" + symbol + "_" + interval + "_ppo_pro.json";
    std::ofstream(model_path) << result.dump(2);

    std::cout << "[TRAIN] rows=" << jget(metrics,"N_rows")
              << " M_labeled=" << M_labeled
              << " best_thr=" << best_thr
              << " acc=" << val_acc
              << " rew=" << val_reward << std::endl;

    json reply{
        {"ok", true},
        {"tp", tp},
        {"sl", sl},
        {"ma_len", ma_len},
        {"best_thr", best_thr},
        {"metrics", metrics},
        {"model_path", model_path}
    };
    return reply;
}

} // namespace etai
