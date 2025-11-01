#include "infer_policy.h"
#include "features/features.h"
#include "json.hpp"
#include <armadillo>
#include <cmath>
#include <vector>
#include <algorithm>

using json = nlohmann::json;

namespace etai {

// ---------- utils ----------
static inline double clamp(double v, double a, double b){
    if (!std::isfinite(v)) return a;
    return std::max(a, std::min(v, b));
}

// Z-score per column
static arma::mat zscore_cols(const arma::mat& X) {
    arma::mat Z = X;
    const arma::uword D = Z.n_cols;
    for (arma::uword j = 0; j < D; ++j) {
        arma::vec col = Z.col(j);
        double mu = arma::mean(col);
        double sd = arma::stddev(col);
        if (!std::isfinite(sd) || sd < 1e-12) sd = 1.0;
        Z.col(j) = (Z.col(j) - mu) / sd;
    }
    return Z;
}

static double last_sigma_returns_from_raw_close(const arma::mat& raw, size_t lookback=64) {
    if (raw.n_rows < 2) return 0.0;
    const arma::uword N = raw.n_rows;
    const arma::uword s = (N > lookback + 1) ? (N - (lookback + 1)) : 1;
    arma::vec r = arma::zeros(N - s);
    for (arma::uword i = s; i < N; ++i) {
        double c1 = raw(i, 4);
        double c0 = raw(i - 1, 4);
        if (c0 <= 0.0) { r(i - s) = 0.0; continue; }
        r(i - s) = (c1 - c0) / c0;
    }
    double sd = arma::stddev(r);
    if (!std::isfinite(sd)) sd = 0.0;
    return sd;
}

// Build score a = tanh(Wx+b) on any TF raw OHLCV
// Нормализация: если policy.norm.{mu,sd} корректны и совпадают с feat_dim — используем их, иначе — z-score.
static bool policy_score_on_raw(const arma::mat& raw,
                                const json& policy,
                                double& out_score,
                                int& out_feat_dim,
                                bool& out_used_norm)
{
    out_used_norm = false;
    if (raw.n_cols < 6 || raw.n_rows < 60) return false;

    int D = policy.value("feat_dim", 0);
    std::vector<double> wv = policy.value("W", std::vector<double>{});
    std::vector<double> bv = policy.value("b", std::vector<double>{});
    if (D <= 0 || (int)wv.size() != D || (int)bv.size() != 1) return false;

    arma::mat F = build_feature_matrix(raw);
    if ((int)F.n_cols != D || F.n_rows < 2) return false;

    // Попробуем нормализацию из policy.norm
    if (policy.contains("norm") && policy["norm"].is_object()) {
        const json& N = policy["norm"];
        if (N.contains("mu") && N.contains("sd") && N["mu"].is_array() && N["sd"].is_array()
            && (int)N["mu"].size() == D && (int)N["sd"].size() == D)
        {
            // применяем сохранённые mu/sd
            for (int j = 0; j < D; ++j) {
                double mu = N["mu"][j].get<double>();
                double sd = N["sd"][j].get<double>();
                if (!std::isfinite(sd) || sd < 1e-12) sd = 1.0;
                F.col((arma::uword)j) = (F.col((arma::uword)j) - mu) / sd;
            }
            out_used_norm = true;
        }
    }
    // Если norm не применён — z-score на лету (чтобы не падать)
    if (!out_used_norm) {
        F = zscore_cols(F);
    }

    const arma::uword last = F.n_rows - 1;
    arma::vec x = F.row(last).t(); // D×1

    arma::rowvec W(D);
    for (int i = 0; i < D; ++i) W(i) = wv[(size_t)i];
    double b = bv[0];

    double z = arma::as_scalar(W * x + b);
    out_score = std::tanh(z); // [-1,1]
    out_feat_dim = D;
    return true;
}

// ---------- single-TF policy inference ----------
nlohmann::json infer_with_policy(const arma::mat& raw15, const nlohmann::json& model) {
    if (raw15.n_cols < 6 || raw15.n_rows < 60)
        return json{{"ok", false}, {"error", "not_enough_data"}, {"raw_rows", (int)raw15.n_rows}, {"raw_cols", (int)raw15.n_cols}};
    if (!model.is_object() || !model.contains("policy"))
        return json{{"ok", false}, {"error", "no_policy_in_model"}};

    const json& P = model["policy"];
    int D = 0;
    double a15 = 0.0;
    bool used_norm = false;
    if (!policy_score_on_raw(raw15, P, a15, D, used_norm))
        return json{{"ok", false}, {"error", "policy_scoring_failed"}};

    const double act_gate = 0.10;
    std::string sig = "NEUTRAL";
    if (std::abs(a15) >= act_gate) sig = (a15 >= 0.0) ? "LONG" : "SHORT";

    double sigma = last_sigma_returns_from_raw_close(raw15, 64);
    double vol_threshold = 0.001;

    return json{
        {"ok", true},
        {"signal", sig},
        {"score", a15},
        {"sigma", sigma},
        {"vol_threshold", vol_threshold},
        {"used_norm", used_norm},
        {"feat_dim_used", D}
    };
}

// ---------- MTF-aware policy inference ----------
nlohmann::json infer_with_policy_mtf(const arma::mat& raw15,
                                     const nlohmann::json& model,
                                     const arma::mat* raw60,   int /*ma60*/,
                                     const arma::mat* raw240,  int /*ma240*/,
                                     const arma::mat* raw1440, int /*ma1440*/) {
    if (!model.is_object() || !model.contains("policy"))
        return json{{"ok", false}, {"error", "no_policy_in_model"}};
    const json& P = model["policy"];

    int D = 0;
    double s15 = 0.0;
    bool used_norm_15 = false;
    if (!policy_score_on_raw(raw15, P, s15, D, used_norm_15))
        return json{{"ok", false}, {"error", "policy_scoring_failed_15"}};

    auto score_opt = [&](const arma::mat* raw, double& s, bool& used_norm)->bool{
        if (!raw || raw->n_rows==0) return false;
        int Dtmp=0; used_norm=false;
        return policy_score_on_raw(*raw, P, s, Dtmp, used_norm);
    };

    double s60=0.0,s240=0.0,s1440=0.0;
    bool un60=false, un240=false, un1440=false;
    bool has60   = score_opt(raw60,   s60,   un60);
    bool has240  = score_opt(raw240,  s240,  un240);
    bool has1440 = score_opt(raw1440, s1440, un1440);

    auto sgn = [](double x)->int { return (x>0) - (x<0); };

    int s15sgn = sgn(s15);
    int avail = 0, agree = 0;

    json htf = json::object();
    if (has60)   { ++avail; bool ok = (sgn(s60)   == s15sgn); htf["agree60"]  = ok; if (ok) ++agree; }
    if (has240)  { ++avail; bool ok = (sgn(s240)  == s15sgn); htf["agree240"] = ok; if (ok) ++agree; }
    if (has1440) { ++avail; bool ok = (sgn(s1440) == s15sgn); htf["agree1440"]= ok; if (ok) ++agree; }

    double wctx_htf = 1.0;
    if (avail > 0) {
        double frac = (double)agree / (double)avail; // [0..1]
        wctx_htf = 0.75 + 0.25 * frac;               // [0.75..1]
    }

    const double act_gate = 0.10;
    double a_w = s15 * wctx_htf;

    std::string sig = "NEUTRAL";
    if (std::abs(a_w) >= act_gate) sig = (a_w >= 0.0) ? "LONG" : "SHORT";

    double sigma15 = last_sigma_returns_from_raw_close(raw15, 64);
    double vol_threshold = 0.001;

    json out{
        {"ok", true},
        {"signal", sig},
        {"score15", s15},
        {"score_w", a_w},
        {"sigma15", sigma15},
        {"vol_threshold", vol_threshold},
        {"wctx_htf", wctx_htf},
        {"htf", htf},
        {"used_norm", used_norm_15},
        {"feat_dim_used", D}
    };
    if (!has60)   out["htf"]["agree60"]   = nullptr;
    if (!has240)  out["htf"]["agree240"]  = nullptr;
    if (!has1440) out["htf"]["agree1440"] = nullptr;
    return out;
}

} // namespace etai
