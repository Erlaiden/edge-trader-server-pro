#include "infer_policy.h"
#include "features/features.h"
#include <cmath>
#include <vector>

using json = nlohmann::json;

namespace etai {

static inline double clamp(double v, double a, double b){ return std::max(a, std::min(v, b)); }

static arma::mat zscore_rows(const arma::mat& X) {
    arma::mat Z = X;
    for (size_t r=0; r<Z.n_rows; ++r) {
        arma::rowvec row = Z.row(r);
        double mean = arma::mean(row);
        double sd = arma::stddev(row);
        if (sd < 1e-12) sd = 1.0;
        Z.row(r) = (row - mean) / sd;
    }
    return Z;
}

static double last_sigma_returns(const arma::vec& close, size_t lookback=64) {
    if (close.n_elem < 2) return 0.0;
    size_t N = close.n_elem;
    size_t s = (N > lookback+1) ? (N - (lookback+1)) : 1;
    arma::vec r = arma::zeros(N - s);
    for (size_t i=s; i<N; ++i) {
        r(i - s) = (close(i) - close(i-1)) / std::max(close(i-1), 1e-9);
    }
    double sd = arma::stddev(r);
    if (!std::isfinite(sd)) sd = 0.0;
    return sd;
}

nlohmann::json infer_with_policy(const arma::mat& M15, const nlohmann::json& model) {
    // Проверки данных
    if (M15.n_rows < 5 || M15.n_cols < 10)
        return json{{"ok", false}, {"error", "not_enough_data"}};

    if (!model.contains("policy"))
        return json{{"ok", false}, {"error", "no_policy_in_model"}};

    const json& P = model["policy"];
    if (!P.contains("W") || !P.contains("b") || !P.contains("feat_dim"))
        return json{{"ok", false}, {"error", "policy_fields_missing"}};

    int D = P.value("feat_dim", 0);
    std::vector<double> wv = P.value("W", std::vector<double>{});
    std::vector<double> bv = P.value("b", std::vector<double>{});

    if (D <= 0 || (int)wv.size() != D || (int)bv.size() != 1)
        return json{{"ok", false}, {"error", "policy_shape_mismatch"}};

    // Признаки
    arma::mat X = build_feature_matrix(M15); // D x N (ожидаем тот же набор признаков)
    if ((int)X.n_rows != D || X.n_cols < 2)
        return json{{"ok", false}, {"error", "feature_dim_mismatch_or_short"}};

    X = zscore_rows(X);
    arma::vec x = X.col(X.n_cols - 1); // последний бар

    // Параметры policy
    arma::rowvec W(D); for (int i=0;i<D;++i) W(i) = wv[(size_t)i];
    double b = bv[0];

    double z = arma::as_scalar(W * x + b);
    double a = std::tanh(z);            // [-1, 1]
    double act_gate = 0.10;             // тот же порог, что и в обучении
    std::string sig = "NEUTRAL";
    if (std::abs(a) >= act_gate)
        sig = (a >= 0.0) ? "LONG" : "SHORT";

    // Оценка волы (для справки и UI-порогов)
    arma::vec close = M15.row(4).t();
    double sigma = last_sigma_returns(close, 64);
    double vol_threshold = 0.001; // как и раньше — константа согласованности UI

    return json{
        {"ok", true},
        {"signal", sig},
        {"score", a},
        {"sigma", sigma},
        {"vol_threshold", vol_threshold}
    };
}

} // namespace etai
