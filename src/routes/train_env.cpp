#include <cstdlib>
#include <fstream>
#include <armadillo>
#include <httplib.h>
#include "json.hpp"

#include "utils_data.h"
#include "features.h"
#include "env/env_trading.h"
#include "env/episode_runner.h"

using json = nlohmann::json;

// ---------- helpers ----------
static inline bool feature_on(const char* k) {
    const char* s = std::getenv(k);
    if (!s || !*s) return false;
    return (s[0]=='1') || (s[0]=='T') || (s[0]=='t') || (s[0]=='Y') || (s[0]=='y');
}
static inline double sigmoid(double z) {
    if (!std::isfinite(z)) z = 0.0;
    return 1.0 / (1.0 + std::exp(-z));
}
// локальные метрики
static inline double local_sharpe(const arma::vec& pnl, double eps=1e-12) {
    if (pnl.n_elem == 0) return 0.0;
    double mu = arma::mean(pnl);
    double sd = arma::stddev(pnl);
    if (!std::isfinite(sd) || sd < eps) sd = eps;
    return mu / sd;
}
static inline double local_max_drawdown(const arma::vec& pnl) {
    if (pnl.n_elem == 0) return 0.0;
    arma::vec eq(pnl.n_elem + 1, arma::fill::ones);
    for (arma::uword i = 0; i < pnl.n_elem; ++i) eq(i+1) = eq(i) + pnl(i);
    double peak = eq(0), maxdd = 0.0;
    for (arma::uword i = 1; i < eq.n_elem; ++i) {
        if (eq(i) > peak) peak = eq(i);
        if (peak > 0.0) {
            double dd = (peak - eq(i)) / peak;
            if (dd > maxdd) maxdd = dd;
        }
    }
    return maxdd;
}
static inline double local_winrate(const arma::vec& pnl) {
    if (pnl.n_elem == 0) return 0.0;
    arma::uword pos = 0, den = 0;
    for (arma::uword i = 0; i < pnl.n_elem; ++i) {
        if (pnl(i) > 0) { ++pos; ++den; }
        else if (pnl(i) < 0) { ++den; }
    }
    return (den > 0) ? (double)pos / (double)den : 0.0;
}

// ---------- модельная политика ----------
struct ModelPolicy {
    std::vector<double> W;
    double b = 0.0;
    double thr = 0.5;
    int feat_dim = 0;
    bool ok = false;

    inline int operator()(const std::vector<double>& state) const {
        if (!ok || (int)state.size() != feat_dim) return 0;
        double z = b;
        for (int j = 0; j < feat_dim; ++j) z += W[j] * state[j];
        double p = sigmoid(z);
        return (p >= thr) ? +1 : -1;
    }
};

static ModelPolicy load_model_policy(const std::string& path) {
    ModelPolicy mp;
    try {
        std::ifstream f(path);
        if (!f.good()) return mp;
        std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        json j = json::parse(s);

        if (!j.contains("policy") || !j["policy"].is_object()) return mp;
        auto& pol = j["policy"];
        if (!pol.contains("W") || !pol["W"].is_array()) return mp;

        mp.W.reserve(pol["W"].size());
        for (auto& v : pol["W"]) mp.W.push_back(v.get<double>());

        if (pol.contains("b")) {
            if (pol["b"].is_array() && !pol["b"].empty()) mp.b = pol["b"][0].get<double>();
            else if (pol["b"].is_number()) mp.b = pol["b"].get<double>();
        }
        if (pol.contains("feat_dim")) mp.feat_dim = pol["feat_dim"].get<int>();
        if (j.contains("best_thr"))   mp.thr = j["best_thr"].get<double>();

        mp.ok = (int)mp.W.size() == mp.feat_dim && mp.feat_dim > 0;
        return mp;
    } catch (...) {
        return mp;
    }
}

// ---------- роут ----------
static inline void register_train_env_routes(httplib::Server& svr) {
    svr.Get("/api/train_env", [](const httplib::Request& req, httplib::Response& res) {
        if (!feature_on("ETAI_ENABLE_TRAIN_ENV")) {
            json j = {{"ok", false}, {"error", "feature_disabled"}, {"hint", "export ETAI_ENABLE_TRAIN_ENV=1"}, {"version", "env_v1"}};
            res.set_content(j.dump(2), "application/json");
            return;
        }

        try {
            const std::string symbol = req.has_param("symbol") ? req.get_param_value("symbol") : "BTCUSDT";
            const std::string interval = req.has_param("interval") ? req.get_param_value("interval") : "15";
            int steps = 500;
            try { if (req.has_param("steps")) steps = std::stoi(req.get_param_value("steps")); } catch(...) {}

            // 1) Данные и фичи
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

            // 2) Политика из текущей модели
            ModelPolicy mp = load_model_policy("cache/models/BTCUSDT_15_ppo_pro.json");
            if (!mp.ok) {
                json err = {{"ok", false}, {"error", "no_policy"}, {"detail", "model json missing or invalid"}};
                res.set_content(err.dump(2), "application/json");
                return;
            }

            // 2a) Толерантность к рассинхрону фич (за флагом)
            bool adapted = false;
            if (mp.feat_dim != (int)Fm.n_cols) {
                if (feature_on("ETAI_TOLERATE_FEAT_MISMATCH") &&
                    (int)Fm.n_cols + 4 == mp.feat_dim)
                {
                    arma::mat Fa(Fm.n_rows, mp.feat_dim, arma::fill::zeros);
                    Fa.cols(0, Fm.n_cols-1) = Fm; // дополняем 4 нулями (место под Money Flow)
                    Fm = std::move(Fa);
                    adapted = true;
                } else {
                    json err = {
                        {"ok", false},
                        {"error", "feat_dim_mismatch"},
                        {"policy_feat_dim", mp.feat_dim},
                        {"features_cols", (int)Fm.n_cols},
                        {"hint", "export ETAI_FEAT_ENABLE_MFLOW=1  (или ETAI_TOLERATE_FEAT_MISMATCH=1 для нулевого паддинга)"}
                    };
                    res.set_content(err.dump(2), "application/json");
                    return;
                }
            }

            // 3) Конвертация в std::vector
            std::vector<std::vector<double>> feats(Fm.n_rows, std::vector<double>(Fm.n_cols));
            for (size_t i = 0; i < Fm.n_rows; ++i)
                for (size_t j = 0; j < Fm.n_cols; ++j)
                    feats[i][j] = Fm(i, j);
            std::vector<double> closes(Fm.n_rows);
            for (size_t i = 0; i < Fm.n_rows; ++i) closes[i] = raw(i,4);

            etai::EnvConfig cfg;
            cfg.start_equity = 1.0;
            cfg.fee_per_trade = 0.0005;
            cfg.feat_dim = (int)Fm.n_cols;

            etai::EnvTrading env;
            env.set_dataset(feats, closes, cfg);

            auto policy_fn = [mp](const std::vector<double>& st)->int { return mp(st); };

            etai::EpisodeRunner runner;
            auto traj = runner.run_fixed(env, policy_fn, std::min<int>(steps, (int)Fm.n_rows-1));

            // 4) Метрики по pnl (r_t из env)
            arma::vec pnl(traj.rewards.size());
            for (size_t i = 0; i < traj.rewards.size(); ++i) pnl(i) = traj.rewards[i];

            double sharpe  = local_sharpe(pnl, 1e-12);
            double dd_max  = local_max_drawdown(pnl);
            double winrate = local_winrate(pnl);

            double pos_sum = 0.0, neg_sum = 0.0;
            for (double r : traj.rewards) {
                if (r > 0) pos_sum += r;
                else if (r < 0) neg_sum += r;
            }
            double pf = (pos_sum > 0 && neg_sum < 0) ? (pos_sum / std::abs(neg_sum)) : 0.0;

            json out;
            out["ok"] = true;
            out["env"] = "v1";
            out["rows"] = (int)Fm.n_rows;
            out["cols"] = (int)Fm.n_cols;
            out["steps"] = (int)traj.steps;
            out["policy"] = {{"source","model_json"},{"thr", mp.thr},{"feat_dim", mp.feat_dim}};
            out["equity_final"] = traj.equity_final;
            out["max_dd"] = dd_max;
            out["max_dd_env"] = traj.max_dd;
            out["winrate"] = winrate;
            out["pf"] = pf;
            out["sharpe"] = sharpe;
            out["wins"] = traj.wins;
            out["losses"] = traj.losses;
            out["feat_adapted"] = adapted;

            res.set_content(out.dump(2), "application/json");
        } catch (...) {
            json err = {{"ok", false}, {"error", "exception"}};
            res.set_content(err.dump(2), "application/json");
        }
    });
}
