#include "train_logic.h"
#include "server_accessors.h"   // etai::{get_data_health,set_model_thr,set_model_ma_len,set_current_model}
#include "ppo_pro.h"            // etai::trainPPO_pro(...)
#include "utils_data.h"         // etai::load_cached_xy(...)
#include "json.hpp"

#include <armadillo>
#include <fstream>
#include <mutex>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <cmath>

using json = nlohmann::json;

namespace etai {

using arma::mat;

static std::mutex train_mutex;

static inline long long now_ms() {
    return static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
}

nlohmann::json run_train_pro_and_save(const std::string& symbol,
                                      const std::string& interval,
                                      int episodes,
                                      double tp,
                                      double sl,
                                      int ma_len)
{
    std::lock_guard<std::mutex> lk(train_mutex);

    json out = {
        {"ok", false},
        {"model_path", nullptr},
        {"metrics", json::object()},
        {"error", nullptr},
        {"error_detail", nullptr}
    };

    try {
        // 1) sanity по данным
        json dh = etai::get_data_health();
        if (!dh.value("has_data", false)) {
            throw std::runtime_error("no training data available");
        }

        arma::mat X, y;
        if (!etai::load_cached_xy(symbol, interval, X, y)) {
            throw std::runtime_error("failed to load cached XY");
        }
        if (X.n_rows == 0 || X.n_cols == 0) {
            throw std::runtime_error("empty feature matrix");
        }

        std::cout << "[TRAIN] PPO_PRO start rows=" << X.n_rows
                  << " cols=" << X.n_cols
                  << " episodes=" << episodes
                  << " tp=" << tp << " sl=" << sl
                  << " ma=" << ma_len << std::endl;

        // 2) обучение (валидационные матрицы не передаем — тренер сам подготовит внутри при необходимости)
        json metrics = etai::trainPPO_pro(X, nullptr, nullptr, nullptr, episodes, tp, sl, ma_len);

        // 3) извлекаем ключевые метрики
        double best_thr = metrics.value("best_thr", 0.001);
        if (!(std::isfinite(best_thr) && best_thr > 0.0 && best_thr < 1.0)) {
            best_thr = 0.001; // страховка от NaN/Inf/мусора
        }
        double val_acc = metrics.value("val_accuracy", 0.0);
        double val_rew = metrics.value("val_reward", 0.0);

        // 4) собираем JSON модели (версия 3, schema ppo_pro_v1, feat_version=2)
        json model = {
            {"version", 3},
            {"schema", "ppo_pro_v1"},
            {"mode", "pro"},
            {"best_thr", best_thr},
            {"ma_len", ma_len},
            {"tp", tp},
            {"sl", sl},
            {"build_ts", now_ms()},
            {"policy", {
                {"feat_dim", static_cast<int>(X.n_cols)},
                {"feat_version", 2}
            }},
            {"metrics", metrics}
        };

        // 5) сохраняем на диск
        const std::string out_path = "cache/models/" + symbol + "_" + interval + "_ppo_pro.json";
        {
            std::ofstream ofs(out_path);
            if (!ofs) throw std::runtime_error("failed to open model file for write: " + out_path);
            ofs << model.dump(2);
        }

        // 6) обновляем атомы + текущую модель для health/metrics
        etai::set_model_thr(best_thr);
        etai::set_model_ma_len(ma_len);
        etai::set_current_model(model);

        out["ok"] = true;
        out["model_path"] = out_path;
        out["metrics"] = {
            {"best_thr", best_thr},
            {"val_accuracy", val_acc},
            {"val_reward", val_rew}
        };
        return out;
    } catch (const std::exception& e) {
        out["error"] = "train_pro_exception";
        out["error_detail"] = e.what();
        return out;
    } catch (...) {
        out["error"] = "train_pro_unknown";
        out["error_detail"] = "unknown error";
        return out;
    }
}

} // namespace etai
