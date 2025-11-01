#include "utils_data.h"
#include "features.h"
#include "json.hpp"
#include <httplib.h>
#include <armadillo>
#include <iostream>
#include <cstdlib>

using json = nlohmann::json;
using namespace httplib;

namespace etai {
struct EnvTrading {
    arma::mat feats;
    double balance = 1.0;
    double commission = 0.0005;
    int steps = 0;

    EnvTrading(const arma::mat& F) : feats(F) {}

    json rollout(int limit = 200) {
        int N = feats.n_rows;
        int D = feats.n_cols;
        int T = std::min(N, limit);
        double equity = balance;
        for (int i = 0; i < T; ++i) {
            double action = feats(i, 0); // proxy: первый фича канал как сигнал
            double reward = action * 0.001; // символический
            equity += reward - commission;
        }
        json j;
        j["steps"] = T;
        j["feat_dim"] = D;
        j["final_equity"] = equity;
        j["commission"] = commission;
        return j;
    }
};
} // namespace etai

void register_train_env(Server& svr) {
    svr.Get("/api/train_env", [](const Request& req, Response& res) {
        if (!std::getenv("ETAI_ENABLE_TRAIN_ENV")) {
            res.set_content(R"({"ok":false,"error":"feature_disabled"})", "application/json");
            return;
        }

        try {
            arma::mat raw;
            if (!etai::load_raw_ohlcv("BTCUSDT", "15", raw)) {
                json err = {{"ok", false}, {"error", "failed_load_raw"}};
                res.set_content(err.dump(2), "application/json");
                return;
            }

            arma::mat F = etai::build_feature_matrix(raw);
            etai::EnvTrading env(F);
            json rollout = env.rollout(300);

            json out;
            out["ok"] = true;
            out["env_version"] = "v1";
            out["rollout"] = rollout;
            out["rows"] = F.n_rows;
            out["cols"] = F.n_cols;
            res.set_content(out.dump(2), "application/json");
        } catch (const std::exception& e) {
            json err = {{"ok", false}, {"error", e.what()}};
            res.set_content(err.dump(2), "application/json");
        }
    });
}
