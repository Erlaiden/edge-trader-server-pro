#include <cstdlib>
#include <armadillo>
#include <httplib.h>
#include "json.hpp"

#include "utils_data.h"
#include "features.h"
#include "env/env_trading.h"
#include "env/episode_runner.h"

using json = nlohmann::json;

// фичефлаг
static inline bool feature_on(const char* k) {
    const char* s = std::getenv(k);
    if (!s || !*s) return false;
    return (s[0]=='1') || (s[0]=='T') || (s[0]=='t') || (s[0]=='Y') || (s[0]=='y');
}

// простая политика: знак первого канала фич
static inline int sign_policy(const std::vector<double>& s) {
    if (s.empty()) return 0;
    double x = s[0];
    if (x >  1e-12) return +1;
    if (x < -1e-12) return -1;
    return 0;
}

static inline void register_train_env_routes(httplib::Server& svr) {
    svr.Get("/api/train_env", [](const httplib::Request& req, httplib::Response& res) {
        if (!feature_on("ETAI_ENABLE_TRAIN_ENV")) {
            json j = {{"ok", false}, {"error", "feature_disabled"}, {"hint", "export ETAI_ENABLE_TRAIN_ENV=1"}, {"version", "env_v1"}};
            res.set_content(j.dump(2), "application/json");
            return;
        }

        try {
            const std::string symbol = "BTCUSDT";
            const std::string interval = "15";
            int steps = 500;
            try { if (req.has_param("steps")) steps = std::stoi(req.get_param_value("steps")); } catch(...) {}

            arma::mat raw;
            if (!etai::load_raw_ohlcv(symbol, interval, raw)) {
                json err = {{"ok", false}, {"error", "failed_load_raw"}};
                res.set_content(err.dump(2), "application/json");
                return;
            }

            arma::mat Fm = etai::build_feature_matrix(raw);
            if (Fm.n_rows == 0 || Fm.n_cols == 0) {
                json err = {{"ok", false}, {"error", "empty_features"}};
                res.set_content(err.dump(2), "application/json");
                return;
            }

            // конвертируем в std::vector для env
            std::vector<std::vector<double>> feats(Fm.n_rows, std::vector<double>(Fm.n_cols));
            for (size_t i = 0; i < Fm.n_rows; ++i)
                for (size_t j = 0; j < Fm.n_cols; ++j)
                    feats[i][j] = Fm(i, j);

            std::vector<double> closes(Fm.n_rows);
            for (size_t i = 0; i < Fm.n_rows; ++i) closes[i] = raw(i,4);

            etai::EnvConfig cfg;
            cfg.start_equity = 1.0;
            cfg.fee_per_trade = 0.0005;
            cfg.feat_dim = static_cast<int>(Fm.n_cols);

            etai::EnvTrading env;
            env.set_dataset(feats, closes, cfg);

            etai::EpisodeRunner runner;
            auto traj = runner.run_fixed(env, sign_policy, std::min<int>(steps, (int)Fm.n_rows-1));

            // агрегаты
            double sum = 0.0;
            for (double r : traj.rewards) sum += r;
            double avg = traj.rewards.empty() ? 0.0 : sum / (double)traj.rewards.size();

            json out;
            out["ok"] = true;
            out["env"] = "v1";
            out["rows"] = Fm.n_rows;
            out["cols"] = Fm.n_cols;
            out["steps"] = traj.steps;
            out["equity_final"] = traj.equity_final;
            out["max_dd"] = traj.max_dd;
            out["win"] = traj.wins;
            out["loss"] = traj.losses;
            out["avg_reward"] = avg;
            res.set_content(out.dump(2), "application/json");
        } catch (...) {
            json err = {{"ok", false}, {"error", "exception"}};
            res.set_content(err.dump(2), "application/json");
        }
    });
}
