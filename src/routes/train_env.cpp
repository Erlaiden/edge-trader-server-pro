#include <cstdlib>
#include <iostream>
#include <armadillo>
#include <httplib.h>
#include "json.hpp"

#include "utils_data.h"
#include "features.h"

using json = nlohmann::json;

// единая проверка фичефлага
static inline bool feature_on(const char* k) {
    const char* s = std::getenv(k);
    if (!s || !*s) return false;
    return (s[0]=='1') || (s[0]=='T') || (s[0]=='t') || (s[0]=='Y') || (s[0]=='y');
}

// Простая безопасная обвязка для пробного rollout.
// Здесь НЕ меняем модель и не пишем на диск.
static json rollout_dummy(const arma::mat& F, int limit_steps) {
    const int N = static_cast<int>(F.n_rows);
    const int D = static_cast<int>(F.n_cols);
    const int T = std::max(0, std::min(N, limit_steps));

    double equity = 1.0;
    const double commission = 0.0005;

    for (int i = 0; i < T; ++i) {
        // прокси-сигнал: первый канал фич
        const double signal = (D > 0) ? F(i, 0) : 0.0;
        const double reward = signal * 0.001; // символический
        equity += reward - commission;
    }

    json j;
    j["steps"] = T;
    j["feat_dim"] = D;
    j["final_equity"] = equity;
    j["commission"] = commission;
    return j;
}

// ПУБЛИЧНАЯ ФУНКЦИЯ РЕГИСТРАЦИИ (как ждёт main.cpp)
static inline void register_train_env_routes(httplib::Server& svr) {
    svr.Get("/api/train_env", [](const httplib::Request& req, httplib::Response& res) {
        if (!feature_on("ETAI_ENABLE_TRAIN_ENV")) {
            json j = {
                {"ok", false},
                {"error", "feature_disabled"},
                {"hint", "export ETAI_ENABLE_TRAIN_ENV=1"},
                {"version", "env_v1_rfc"}
            };
            res.set_content(j.dump(2), "application/json");
            return;
        }

        try {
            // параметры по умолчанию
            const std::string symbol   = "BTCUSDT";
            const std::string interval = "15";
            int max_steps = 300;
            // allow ?steps=...
            try { max_steps = std::stoi(req.get_param_value("steps")); } catch(...) {}

            arma::mat raw;
            if (!etai::load_raw_ohlcv(symbol, interval, raw)) {
                json err = {{"ok", false}, {"error", "failed_load_raw"}};
                res.set_content(err.dump(2), "application/json");
                return;
            }

            arma::mat F = etai::build_feature_matrix(raw);
            if (F.n_rows == 0 || F.n_cols == 0) {
                json err = {{"ok", false}, {"error", "empty_features"}};
                res.set_content(err.dump(2), "application/json");
                return;
            }

            json roll = rollout_dummy(F, max_steps);

            json out;
            out["ok"] = true;
            out["env"] = "v1_stub";
            out["rows"] = F.n_rows;
            out["cols"] = F.n_cols;
            out["rollout"] = roll;
            res.set_content(out.dump(2), "application/json");
        } catch (const std::exception& e) {
            json err = {{"ok", false}, {"error", "exception"}, {"detail", e.what()}};
            res.set_content(err.dump(2), "application/json");
        } catch (...) {
            json err = {{"ok", false}, {"error", "unknown"}};
            res.set_content(err.dump(2), "application/json");
        }
    });
}
