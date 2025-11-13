#include "infer_policy.h"
#include "features/features.h"
#include "json.hpp"
#include <armadillo>
#include <cmath>
#include <vector>
#include <algorithm>
#include <iostream>

using json = nlohmann::json;

namespace etai {

static inline double clamp(double v, double a, double b){
    if (!std::isfinite(v)) return a;
    return std::max(a, std::min(v, b));
}

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

static bool extract_policy_norm(const json& policy, arma::vec& mu, arma::vec& sd, int D){
    if (!policy.contains("norm") || !policy["norm"].is_object()) return false;
    const json& n = policy["norm"];
    if (!n.contains("mu") || !n.contains("sd")) return false;
    if (!n["mu"].is_array() || !n["sd"].is_array()) return false;
    if ((int)n["mu"].size() != D || (int)n["sd"].size() != D) return false;
    
    mu.set_size(D); sd.set_size(D);
    for (int i = 0; i < D; ++i) {
        mu(i) = n["mu"][i].get<double>();
        sd(i) = n["sd"][i].get<double>();
        if (!std::isfinite(sd(i)) || sd(i) < 1e-12) sd(i) = 1.0;
    }
    return true;
}

static void apply_norm_inplace(arma::mat& X, const arma::vec& mu, const arma::vec& sd){
    const arma::uword D = X.n_cols;
    for (arma::uword j = 0; j < D; ++j) {
        X.col(j) = (X.col(j) - mu(j)) / sd(j);
    }
}

// КРИТИЧНО: обернуто в try-catch
static bool policy_score_on_raw(const arma::mat& raw,
                                const json& policy,
                                double& out_score,
                                int& out_feat_dim,
                                bool& out_used_norm)
{
    try {
        out_used_norm = false;
        if (raw.n_cols < 6 || raw.n_rows < 60) {
            std::cerr << "[POLICY] Bad raw shape: " << raw.n_rows << "x" << raw.n_cols << std::endl;
            return false;
        }

        int D = policy.value("feat_dim", 0);
        std::vector<double> wv = policy.value("W", std::vector<double>{});
        std::vector<double> bv = policy.value("b", std::vector<double>{});
        
        if (D <= 0 || (int)wv.size() != D || (int)bv.size() != 1) {
            std::cerr << "[POLICY] Invalid policy params: D=" << D << " W.size=" << wv.size() << " b.size=" << bv.size() << std::endl;
            return false;
        }

        arma::mat F;
        try {
            F = build_feature_matrix(raw);
        } catch (const std::exception& e) {
            std::cerr << "[POLICY] build_feature_matrix exception: " << e.what() << std::endl;
            return false;
        } catch (...) {
            std::cerr << "[POLICY] build_feature_matrix unknown exception" << std::endl;
            return false;
        }

        if ((int)F.n_cols != D || F.n_rows < 2) {
            std::cerr << "[POLICY] Feature mismatch: expected " << D << " cols, got " << F.n_cols 
                      << " (rows=" << F.n_rows << ")" << std::endl;
            return false;
        }

        arma::vec mu, sd;
        if (extract_policy_norm(policy, mu, sd, D)) {
            apply_norm_inplace(F, mu, sd);
            out_used_norm = true;
        } else {
            F = zscore_cols(F);
            out_used_norm = false;
        }

        const arma::uword last = F.n_rows - 1;
        arma::vec x = F.row(last).t();
        arma::rowvec W(D);
        for (int i = 0; i < D; ++i) W(i) = wv[(size_t)i];
        double b = bv[0];

        double z = arma::as_scalar(W * x + b);
        out_score = std::tanh(z);
        out_feat_dim = D;
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "[POLICY] Exception in policy_score_on_raw: " << e.what() << std::endl;
        return false;
    }
    catch (...) {
        std::cerr << "[POLICY] Unknown exception in policy_score_on_raw" << std::endl;
        return false;
    }
}

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

nlohmann::json infer_with_policy_mtf(const arma::mat& raw15,
                                     const nlohmann::json& model,
                                     const arma::mat* raw60,   int /*ma60*/,
                                     const arma::mat* raw240,  int /*ma240*/,
                                     const arma::mat* raw1440, int /*ma1440*/)
{
    if (!model.is_object() || !model.contains("policy"))
        return json{{"ok", false}, {"error", "no_policy_in_model"}};
    const json& P = model["policy"];

    int D = 0;
    double s15 = 0.0;
    bool used_norm_15 = false;
    if (!policy_score_on_raw(raw15, P, s15, D, used_norm_15))
        return json{{"ok", false}, {"error", "policy_scoring_failed_15"}};

    double s60 = 0.0, s240 = 0.0, s1440 = 0.0;
    bool used_norm_60=false, used_norm_240=false, used_norm_1440=false;
    bool has60   = raw60   && raw60->n_elem   && policy_score_on_raw(*raw60,   P, s60,   D, used_norm_60);
    bool has240  = raw240  && raw240->n_elem  && policy_score_on_raw(*raw240,  P, s240,  D, used_norm_240);
    bool has1440 = raw1440 && raw1440->n_elem && policy_score_on_raw(*raw1440, P, s1440, D, used_norm_1440);

    auto sgn = [](double x)->int { return (x>0) - (x<0); };
    int s15sgn = sgn(s15);
    int avail = 0, agree = 0;

    json htf = json::object();
    auto push_htf = [&](const char* key, bool present, double score){
        if (!present) { htf[key] = json{{"agree", nullptr},{"score", nullptr},{"strong", nullptr},{"eps", 0.0}}; return; }
        bool ok = (sgn(score) == s15sgn);
        double eps = std::abs(score);
        bool strong = eps >= 0.3;
        htf[key] = json{{"agree", ok},{"score", score},{"strong", strong},{"eps", eps}};
        ++avail; if (ok) ++agree;
    };

    push_htf("60",   has60,   s60);
    push_htf("240",  has240,  s240);
    push_htf("1440", has1440, s1440);

    double wctx_htf = 1.0;
    if (avail > 0) {
        double frac = (double)agree / (double)avail;
        wctx_htf = 0.75 + 0.25 * frac;
    }

    const double act_gate = 0.10;
    double a_w = s15 * wctx_htf;

    std::string sig = "NEUTRAL";
    if (std::abs(a_w) >= act_gate) sig = (a_w >= 0.0) ? "LONG" : "SHORT";

    double sigma15 = last_sigma_returns_from_raw_close(raw15, 64);
    double vol_threshold = 0.001;
    bool used_norm_any = used_norm_15 || used_norm_60 || used_norm_240 || used_norm_1440;

    json out{
        {"ok", true},
        {"signal", sig},
        {"score15", s15},
        {"score_w", a_w},
        {"sigma15", sigma15},
        {"vol_threshold", vol_threshold},
        {"wctx_htf", wctx_htf},
        {"htf", htf},
        {"used_norm", used_norm_any},
        {"feat_dim_used", D}
    };
    return out;
}

} // namespace etai
