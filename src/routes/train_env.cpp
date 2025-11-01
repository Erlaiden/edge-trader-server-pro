#include <cstdlib>
#include <fstream>
#include <functional>
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
static inline double env_double(const char* k, double defv){
    try{
        const char* s = std::getenv(k);
        if(!s||!*s) return defv;
        return std::stod(s);
    }catch(...){ return defv; }
}
static inline std::string env_str(const char* k, const char* defv){
    const char* s = std::getenv(k);
    if(!s||!*s) return std::string(defv);
    return std::string(s);
}
static inline double clampd(double v,double lo,double hi){
    if(!std::isfinite(v)) return lo;
    if(v<lo) return lo;
    if(v>hi) return hi;
    return v;
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

// «знак тренда» из первой колонки фич (ema_fast - ema_slow)
static inline int trend_sign_from_features(const arma::mat& F, arma::uword last_k=200){
    if (F.n_rows==0 || F.n_cols==0) return 0;
    arma::uword n = F.n_rows;
    arma::uword from = (n>last_k)? (n-last_k): 0;
    arma::vec col = F.col(0).rows(from, n-1);
    double m = arma::mean(col);
    if (m> 1e-12) return +1;
    if (m<-1e-12) return -1;
    return 0;
}
static inline int agree_sign(int a,int b){ if(a==0||b==0) return 0; return (a==b)? +1 : -1; }

// ---------- модельная политика ----------
struct ModelPolicy {
    std::vector<double> W;
    double b = 0.0;
    double thr = 0.5;  // best_thr
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
            const std::string symbol   = req.has_param("symbol")   ? req.get_param_value("symbol")   : "BTCUSDT";
            const std::string interval = req.has_param("interval") ? req.get_param_value("interval") : "15";
            int steps = 500;
            try { if (req.has_param("steps")) steps = std::stoi(req.get_param_value("steps")); } catch(...) {}

            // policy: model|thr_only|sign_channel
            std::string policy = req.has_param("policy") ? req.get_param_value("policy") : "model";
            if (policy!="model" && policy!="thr_only" && policy!="sign_channel") policy="model";

            // fee (комиссия на сделку, абсолютная), клауза безопасности
            double fee = 0.0005;
            try { if (req.has_param("fee")) fee = std::stod(req.get_param_value("fee")); } catch(...) {}
            fee = clampd(fee, 0.0, 0.01);

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

            // 2) Политика
            ModelPolicy mp = load_model_policy("cache/models/BTCUSDT_15_ppo_pro.json");
            bool adapted = false;
            if (policy=="model") {
                if (!mp.ok) {
                    json err = {{"ok", false}, {"error", "no_policy"}, {"detail", "model json missing or invalid"}};
                    res.set_content(err.dump(2), "application/json");
                    return;
                }
                if (mp.feat_dim != (int)Fm.n_cols) {
                    if (feature_on("ETAI_TOLERATE_FEAT_MISMATCH") &&
                        (int)Fm.n_cols + 4 == mp.feat_dim)
                    {
                        arma::mat Fa(Fm.n_rows, mp.feat_dim, arma::fill::zeros);
                        Fa.cols(0, Fm.n_cols-1) = Fm; // дополняем 4 нулями (Money Flow)
                        Fm = std::move(Fa);
                        adapted = true;
                    } else {
                        json err = {
                            {"ok", false}, {"error", "feat_dim_mismatch"},
                            {"policy_feat_dim", mp.feat_dim},
                            {"features_cols", (int)Fm.n_cols},
                            {"hint", "export ETAI_FEAT_ENABLE_MFLOW=1  (или ETAI_TOLERATE_FEAT_MISMATCH=1)"}
                        };
                        res.set_content(err.dump(2), "application/json");
                        return;
                    }
                }
            }

            // 3) Конвертация датасета -> vectors
            std::vector<std::vector<double>> feats(Fm.n_rows, std::vector<double>(Fm.n_cols));
            for (size_t i = 0; i < Fm.n_rows; ++i)
                for (size_t j = 0; j < Fm.n_cols; ++j)
                    feats[i][j] = Fm(i, j);
            std::vector<double> closes(Fm.n_rows);
            for (size_t i = 0; i < Fm.n_rows; ++i) closes[i] = raw(i,4);

            // 4) HTF-контекст (фиксируем на запуск эпизода)
            auto mtf_one_sign = [&](const char* tf_name)->int{
                arma::mat raw_tf;
                if (!etai::load_raw_ohlcv(symbol, tf_name, raw_tf)) return 0;
                arma::mat F_tf = etai::build_feature_matrix(raw_tf);
                if (F_tf.n_rows==0 || F_tf.n_cols==0) return 0;
                return trend_sign_from_features(F_tf, 400);
            };
            int sign15 = trend_sign_from_features(Fm, 400);
            int sign60 = mtf_one_sign("60");
            int sign240 = mtf_one_sign("240");
            int sign1440 = mtf_one_sign("1440");
            int agree60  = agree_sign(sign15, sign60);
            int agree240 = agree_sign(sign15, sign240);
            int agree1440= agree_sign(sign15, sign1440);

            // 5) Конфиг среды
            etai::EnvConfig cfg;
            cfg.start_equity = 1.0;
            cfg.fee_per_trade = fee;
            cfg.feat_dim = (int)Fm.n_cols;

            etai::EnvTrading env;
            env.set_dataset(feats, closes, cfg);

            // 6) Политики
            std::function<int(const std::vector<double>&)> base_policy;
            if (policy=="model") {
                base_policy = [mp](const std::vector<double>& st)->int { return mp(st); };
            } else if (policy=="thr_only") {
                base_policy = [](const std::vector<double>& st)->int {
                    if (st.empty()) return 0;
                    double p = sigmoid(st[0]);
                    return (p >= 0.5) ? +1 : -1;
                };
            } else { // sign_channel
                base_policy = [](const std::vector<double>& st)->int {
                    if (st.empty()) return 0;
                    return (st[0] >= 0.0) ? +1 : -1;
                };
            }

            // 7) МЯГКИЙ КОНТЕКСТ-ГЕЙТИНГ (soft, по умолчанию выключен; включает ETAI_ENABLE_CTX_GATE=1)
            // идея: НЕ блокируем ранние входы; режем только когда оба HTF против и локальная энергия низкая.
            const bool use_gate = feature_on("ETAI_ENABLE_CTX_GATE");
            const std::string gate_mode = env_str("ETAI_CTX_GATE_MODE","soft"); // soft|aggr
            const double e_lo_soft  = clampd(env_double("ETAI_CTX_E_LO", 0.20), 0.0, 1.0);
            const double e_lo_aggr  = clampd(env_double("ETAI_CTX_E_LO_AGGR", 0.35), 0.0, 1.0);
            // индексы фич: energy=7 (см. features.cpp)
            const int IDX_ENERGY = 7;

            struct GateStats { int checked=0, skipped=0; int allowed=0; } gstat;

            auto gated_policy = [&](const std::vector<double>& st)->int {
                int a = base_policy(st); // -1/0/+1
                if (!use_gate || st.size()<=IDX_ENERGY) { gstat.allowed++; return a; }

                double energy = st[IDX_ENERGY]; // ожидается ~[-1..1] или [0..1] в нашей нормировке
                // нормируем безопасно в [0..1]
                double e01 = energy;
                if (!std::isfinite(e01)) e01 = 0.0;
                if (e01 < 0.0) e01 = -e01; // модуль энергии: низкая амплитуда -> 0

                gstat.checked++;

                bool both_against = (agree240 == -1 && agree1440 == -1);
                bool any_against  = (agree240 == -1 || agree1440 == -1);

                if (gate_mode=="aggr") {
                    // агрессивный: при ЛЮБОМ несогласии и низкой энергии — HOLD
                    if (any_against && e01 < e_lo_aggr) { gstat.skipped++; return 0; }
                } else {
                    // мягкий: ТОЛЬКО если оба против и энергия низкая — HOLD
                    if (both_against && e01 < e_lo_soft) { gstat.skipped++; return 0; }
                }
                gstat.allowed++;
                return a;
            };

            // 8) Запуск эпизода
            etai::EpisodeRunner runner;
            auto traj = runner.run_fixed(env, gated_policy, std::min<int>(steps, (int)Fm.n_rows-1));

            // 9) Метрики по pnl
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

            // 10) MTF-блок в ответе
            json htf = json::object();
            htf["15"]   = {{"ok",true}, {"sign",sign15}};
            htf["60"]   = {{"ok",true}, {"sign",sign60}};
            htf["240"]  = {{"ok",true}, {"sign",sign240}};
            htf["1440"] = {{"ok",true}, {"sign",sign1440}};
            htf["agree60"]   = agree60;
            htf["agree240"]  = agree240;
            htf["agree1440"] = agree1440;

            // 11) Ответ
            json out;
            out["ok"] = true;
            out["env"] = "v1";
            out["rows"] = (int)Fm.n_rows;
            out["cols"] = (int)Fm.n_cols;
            out["steps"] = (int)traj.steps;
            out["fee"]   = fee;
            out["policy"] = {
                {"name", policy},
                {"source", (policy=="model" ? "model_json" : "derived")},
                {"thr",   (policy=="model" ? mp.thr : 0.5)},
                {"feat_dim", (int)Fm.n_cols}
            };
            out["equity_final"] = traj.equity_final;
            out["max_dd"] = dd_max;
            out["max_dd_env"] = traj.max_dd;
            out["winrate"] = winrate;
            out["pf"] = pf;
            out["sharpe"] = sharpe;
            out["wins"] = traj.wins;
            out["losses"] = traj.losses;
            out["feat_adapted"] = adapted;
            out["htf"] = htf;

            // отчёт по гейтеру
            if (use_gate) {
                out["gate"] = {
                    {"mode", gate_mode},
                    {"checked", gstat.checked},
                    {"skipped", gstat.skipped},
                    {"allowed", gstat.allowed},
                    {"e_lo_soft", e_lo_soft},
                    {"e_lo_aggr", e_lo_aggr}
                };
            }

            res.set_content(out.dump(2), "application/json");
        } catch (...) {
            json err = {{"ok", false}, {"error", "exception"}};
            res.set_content(err.dump(2), "application/json");
        }
    });
}
