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

// Z-score per column (fallback, когда в policy нет norm)
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

// Приводим матрицу признаков к нужной размерности D (обрезка / нулевое дополняние)
static void ensure_feat_dim(arma::mat& F, int D_target) {
    if (D_target <= 0) return;
    const int D = static_cast<int>(F.n_cols);
    if (D == D_target) return;

    if (D > D_target) {
        F = F.cols(0, static_cast<arma::uword>(D_target - 1));
        return;
    }
    // D < D_target: дополним нулевыми колонками
    arma::mat Z(F.n_rows, static_cast<arma::uword>(D_target), arma::fill::zeros);
    if (D > 0) Z.cols(0, static_cast<arma::uword>(D - 1)) = F;
    F = std::move(Z);
}

// Нормализация по policy.norm.{mu,sd} если есть. Вернёт true, если использована нормализация из модели.
static bool apply_norm_from_policy(arma::mat& F, const json& policy, int& out_dim_used) {
    out_dim_used = static_cast<int>(F.n_cols);

    if (!policy.is_object() || !policy.contains("norm") || !policy["norm"].is_object())
        return false;

    const json& PN = policy["norm"];
    if (!PN.contains("mu") || !PN.contains("sd")) return false;
    if (!PN["mu"].is_array() || !PN["sd"].is_array()) return false;

    std::vector<double> mu = PN.value("mu", std::vector<double>{});
    std::vector<double> sd = PN.value("sd", std::vector<double>{});
    int Dn = static_cast<int>(std::min(mu.size(), sd.size()));
    if (Dn <= 0) return false;

    // Подгоняем число колонок под длину mu/sd
    ensure_feat_dim(F, Dn);
    out_dim_used = Dn;

    for (int j = 0; j < Dn; ++j) {
        double m = std::isfinite(mu[(size_t)j]) ? mu[(size_t)j] : 0.0;
        double s = std::isfinite(sd[(size_t)j]) ? sd[(size_t)j] : 1.0;
        if (s < 1e-12) s = 1.0;
        F.col(static_cast<arma::uword>(j)) = (F.col(static_cast<arma::uword>(j)) - m) / s;
    }
    return true;
}

// Построение счёта политики a = tanh(Wx+b) на сырых OHLCV (с учетом нормализации из policy при наличии)
static bool policy_score_on_raw(const arma::mat& raw,
                                const json& policy,
                                double& out_score,
                                int& out_feat_dim)
{
    out_score = 0.0;
    out_feat_dim = 0;

    if (raw.n_cols < 6 || raw.n_rows < 60) return false;
    if (!policy.is_object()) return false;

    // Извлекаем веса
    const int D_decl = policy.value("feat_dim", 0);
    std::vector<double> wv = policy.value("W", std::vector<double>{});
    std::vector<double> bv = policy.value("b", std::vector<double>{});

    if (wv.empty()) return false;
    double b = 0.0;
    if (!bv.empty()) b = bv[0];
    else if (policy.contains("b") && policy["b"].is_number()) b = policy["b"].get<double>();

    // Фичи
    arma::mat F = build_feature_matrix(raw);
    if (F.n_rows < 2 || F.n_cols == 0) return false;

    // Нормализация: сначала пытаемся использовать policy.norm, иначе — локальный z-score
    int D_used_norm = static_cast<int>(F.n_cols);
    bool used_policy_norm = apply_norm_from_policy(F, policy, D_used_norm);

    // Целевая размерность по весам (надёжнее ориентироваться на размер W)
    const int D_w = static_cast<int>(wv.size());
    int D_target = D_w;

    // Если feat_dim прописан, сверим его с D_w — но не ломаемся, берём минимально надёжное
    if (D_decl > 0 && D_decl != D_w) {
        D_target = std::min(D_w, D_decl);
    }

    // Приведём F под D_target
    ensure_feat_dim(F, D_target);
    out_feat_dim = D_target;

    // Если нормировка не была применена из policy, применим локальную (на уже приведённых по колонкам фичах)
    if (!used_policy_norm) {
        F = zscore_cols(F);
    } else {
        // Если norm применена, но её размер не совпал с итоговой целью — повторно подправим (вдруг D_target < D_used_norm)
        if (D_used_norm != D_target) {
            // Уже обрезали/дополнены нулями — этого достаточно; повторная нормализация не нужна.
        }
    }

    // Подготовим W соответствующей длины
    arma::rowvec W(static_cast<arma::uword>(D_target), arma::fill::zeros);
    for (int i = 0; i < D_target; ++i) W(static_cast<arma::uword>(i)) = wv[(size_t)i];

    // Берём последнюю строку признаков
    const arma::uword last = F.n_rows - 1;
    arma::vec x = F.row(last).t(); // D×1

    double z = arma::as_scalar(W * x + b);
    out_score = std::tanh(z); // [-1,1]
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
    if (!policy_score_on_raw(raw15, P, a15, D))
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
        {"feat_dim_used", D},
        {"used_norm", P.contains("norm")}
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

    // 1) score on 15m (required)
    int D15 = 0;
    double s15 = 0.0;
    if (!policy_score_on_raw(raw15, P, s15, D15))
        return json{{"ok", false}, {"error", "policy_scoring_failed_15"}};

    // 2) optional HTF scores
    int Dtmp = 0;
    double s60 = 0.0, s240 = 0.0, s1440 = 0.0;
    bool has60   = raw60   && raw60->n_elem   && policy_score_on_raw(*raw60,   P, s60,   Dtmp);
    bool has240  = raw240  && raw240->n_elem  && policy_score_on_raw(*raw240,  P, s240,  Dtmp);
    bool has1440 = raw1440 && raw1440->n_elem && policy_score_on_raw(*raw1440, P, s1440, Dtmp);

    auto sgn = [](double x)->int { return (x>0) - (x<0); };

    int s15sgn = sgn(s15);
    int avail = 0, agree = 0;

    json htf = json::object();
    if (has60)   { ++avail; bool ok = (sgn(s60)   == s15sgn); htf["agree60"]  = ok; if (ok) ++agree; }
    if (has240)  { ++avail; bool ok = (sgn(s240)  == s15sgn); htf["agree240"] = ok; if (ok) ++agree; }
    if (has1440) { ++avail; bool ok = (sgn(s1440) == s15sgn); htf["agree1440"]= ok; if (ok) ++agree; }

    // 3) compute soft HTF weight (консервативно: [0.75..1.0])
    double wctx_htf = 1.0;
    if (avail > 0) {
        double frac = (double)agree / (double)avail; // [0..1]
        wctx_htf = 0.75 + 0.25 * frac;
    }

    // 4) weighted decision by HTF context
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
        {"feat_dim_used", D15},
        {"used_norm", P.contains("norm")}
    };
    if (!has60)   out["htf"]["agree60"]   = nullptr;
    if (!has240)  out["htf"]["agree240"]  = nullptr;
    if (!has1440) out["htf"]["agree1440"] = nullptr;
    return out;
}

} // namespace etai
