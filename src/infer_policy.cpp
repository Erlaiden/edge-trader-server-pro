#include "infer_policy.h"
#include "features/features.h"
#include "json.hpp"
#include <armadillo>
#include <cmath>
#include <vector>
#include <algorithm>

using json = nlohmann::json;

namespace etai {

// --- утилиты ---

static inline double clamp(double v, double a, double b){
    if (!std::isfinite(v)) return a;
    return std::max(a, std::min(v, b));
}

// Нормализация по КОЛОНКАМ (каждый признак по всей истории)
static arma::mat zscore_cols(const arma::mat& X) {
    arma::mat Z = X;
    const arma::uword N = Z.n_rows;
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
    // raw: N×6, close = col 4
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

// --- основной инференс ---
// raw15: N×6 OHLCV (ts,open,high,low,close,volume)
// model: объект с полем policy { W[D], b[1], feat_dim=D }
nlohmann::json infer_with_policy(const arma::mat& raw15, const nlohmann::json& model) {
    // 0) Быстрые проверки сырья
    if (raw15.n_cols < 6 || raw15.n_rows < 60) {
        return json{{"ok", false}, {"error", "not_enough_data"}, {"raw_rows", (int)raw15.n_rows}, {"raw_cols", (int)raw15.n_cols}};
    }
    if (!model.is_object() || !model.contains("policy")) {
        return json{{"ok", false}, {"error", "no_policy_in_model"}};
    }

    const json& P = model["policy"];
    if (!P.contains("W") || !P.contains("b") || !P.contains("feat_dim")) {
        return json{{"ok", false}, {"error", "policy_fields_missing"}};
    }

    const int D = P.value("feat_dim", 0);
    std::vector<double> wv = P.value("W", std::vector<double>{});
    std::vector<double> bv = P.value("b", std::vector<double>{});
    if (D <= 0 || (int)wv.size() != D || (int)bv.size() != 1) {
        return json{{"ok", false}, {"error", "policy_shape_mismatch"}, {"feat_dim", D}, {"W_len", (int)wv.size()}, {"b_len", (int)bv.size()}};
    }

    // 1) Строим фичи (N×D) — синхронно с тренером
    arma::mat F = build_feature_matrix(raw15);
    if (F.n_rows < 2 || (int)F.n_cols != D) {
        return json{{"ok", false}, {"error", "feature_dim_mismatch_or_short"}, {"F_rows", (int)F.n_rows}, {"F_cols", (int)F.n_cols}, {"expected_D", D}};
    }

    // 2) Нормализация по колонкам (каждый признак)
    F = zscore_cols(F);

    // 3) Последний бар → вектор признаков (Dx1)
    const arma::uword last = F.n_rows - 1;
    arma::vec x = F.row(last).t(); // D×1

    // 4) Параметры policy
    arma::rowvec W(D);
    for (int i = 0; i < D; ++i) W(i) = wv[(size_t)i];
    double b = bv[0];

    // 5) Скора и сигнал
    double z = arma::as_scalar(W * x + b);
    double a = std::tanh(z);              // [-1, 1]
    const double act_gate = 0.10;         // как в тренере
    std::string sig = "NEUTRAL";
    if (std::abs(a) >= act_gate) sig = (a >= 0.0) ? "LONG" : "SHORT";

    // 6) Оценка волатильности по close из raw
    double sigma = last_sigma_returns_from_raw_close(raw15, 64);
    double vol_threshold = 0.001; // как и раньше — константа для UI

    return json{
        {"ok", true},
        {"signal", sig},
        {"score", a},
        {"sigma", sigma},
        {"vol_threshold", vol_threshold}
    };
}

} // namespace etai
