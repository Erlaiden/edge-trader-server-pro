#include "features.h"
#include "json.hpp"
#include <armadillo>
#include <cmath>
#include <algorithm>
#include <vector>

using json = nlohmann::json;

namespace etai {

constexpr int FEAT_VERSION = 7;

// ====================== Вспомогательные функции ======================
static double ema_one(const std::vector<double>& v, int period, size_t i) {
    if (i + 1 < (size_t)period) return NAN;
    double k = 2.0 / (period + 1);
    double e = v[i - period + 1];
    for (size_t j = i - period + 2; j <= i; ++j)
        e = v[j] * k + e * (1.0 - k);
    return e;
}

static double sma_one(const std::vector<double>& v, int period, size_t i) {
    if (i + 1 < (size_t)period) return NAN;
    double s = 0;
    for (size_t j = i + 1 - period; j <= i; ++j)
        s += v[j];
    return s / period;
}

static double rsi_one(const std::vector<double>& c, int period, size_t i) {
    if (i + 1 < (size_t)period + 1) return NAN;
    double gain = 0, loss = 0;
    for (size_t j = i + 1 - period; j <= i; ++j) {
        double diff = c[j] - c[j - 1];
        if (diff >= 0)
            gain += diff;
        else
            loss -= diff;
    }
    if (loss == 0) return 100.0;
    double rs = gain / loss;
    return 100.0 - (100.0 / (1.0 + rs));
}

static double atr_one(const std::vector<double>& h, const std::vector<double>& l, const std::vector<double>& c, int period, size_t i) {
    if (i + 1 < (size_t)period + 1) return NAN;
    double s = 0;
    for (size_t j = i + 1 - period; j <= i; ++j) {
        double tr = std::max({h[j] - l[j], std::fabs(h[j] - c[j - 1]), std::fabs(l[j] - c[j - 1])});
        s += tr;
    }
    return s / period;
}

static inline double deg(double rad) { return rad * 180.0 / M_PI; }

// ====================== Построение фич ======================
arma::Mat<double> build_feature_matrix(const arma::Mat<double>& raw) {
    if (raw.n_rows < 60 || raw.n_cols < 6) return arma::Mat<double>();

    size_t n = raw.n_rows;
    std::vector<double> open(n), high(n), low(n), close(n), vol(n);
    for (size_t i = 0; i < n; ++i) {
        open[i]  = raw(i, 1);
        high[i]  = raw(i, 2);
        low[i]   = raw(i, 3);
        close[i] = raw(i, 4);
        vol[i]   = raw(i, 5);
    }

    std::vector<double> ema_fast(n), ema_slow(n), rsi_v(n),
        atr_v(n), bb_width(n), volr_v(n),
        ema_angle(n), accel_v(n), atr_rel(n),
        corr_score(n), regime_type(n);

    // === базовые фичи ===
    for (size_t i = 0; i < n; ++i) {
        ema_fast[i] = ema_one(close, 12, i);
        ema_slow[i] = ema_one(close, 26, i);
        rsi_v[i] = rsi_one(close, 14, i);
        atr_v[i] = atr_one(high, low, close, 14, i);

        // BB width
        if (i >= 20) {
            double sma20 = sma_one(close, 20, i);
            double sd = 0;
            for (size_t j = i + 1 - 20; j <= i; ++j)
                sd += std::pow(close[j] - sma20, 2);
            sd = std::sqrt(sd / 20);
            bb_width[i] = (sma20 != 0) ? (4.0 * sd / sma20) : NAN;
        } else bb_width[i] = NAN;

        // Volume ratio
        volr_v[i] = (i >= 20 && sma_one(vol, 20, i) > 0) ? vol[i] / sma_one(vol, 20, i) : NAN;
    }

    // === импульс и ускорение ===
    for (size_t i = 1; i < n; ++i) {
        double slope = ema_fast[i] - ema_fast[i - 1];
        double angle = deg(std::atan(slope / ema_fast[i]));
        ema_angle[i] = angle;
    }
    ema_angle[0] = 0.0;

    for (size_t i = 1; i < n; ++i)
        accel_v[i] = ema_angle[i] - ema_angle[i - 1];
    accel_v[0] = 0.0;

    // === ATR относительный, коррекция ===
    for (size_t i = 0; i < n; ++i) {
        double ema_mid = ema_one(close, 20, i);
        atr_rel[i] = (ema_mid > 0) ? (atr_v[i] / ema_mid) : 0;
        corr_score[i] = (ema_mid > 0) ? ((close[i] - ema_mid) / ema_mid) : 0;
    }

    // === классификация режима рынка ===
    for (size_t i = 0; i < n; ++i) {
        double a = ema_angle[i];
        double ac = accel_v[i];
        double ar = atr_rel[i];

        if (std::fabs(a) < 5 && ar < 0.003)
            regime_type[i] = 0; // flat
        else if (std::fabs(a) > 10 && (a * ac) > 0)
            regime_type[i] = 1; // trend
        else
            regime_type[i] = 2; // correction
    }

    // === матрица признаков ===
    const size_t D = 24;
    arma::Mat<double> F(n, D, arma::fill::zeros);

    for (size_t i = 0; i < n; ++i) {
        F(i, 0) = ema_fast[i] - ema_slow[i];
        F(i, 1) = rsi_v[i] / 100.0;
        F(i, 2) = atr_v[i];
        F(i, 3) = bb_width[i];
        F(i, 4) = volr_v[i];
        F(i, 5) = (close[i] - open[i]) / open[i];
        F(i, 6) = ema_angle[i] / 45.0;
        F(i, 7) = accel_v[i] / 45.0;
        F(i, 8) = atr_rel[i];
        F(i, 9) = corr_score[i];
        F(i,10) = regime_type[i] / 2.0;
        // дублируем связки
        F(i,11) = ema_fast[i] / ema_slow[i];
        F(i,12) = (ema_fast[i] - close[i]) / close[i];
        F(i,13) = (high[i] - low[i]) / close[i];
        F(i,14) = volr_v[i] * bb_width[i];
        F(i,15) = std::fabs(ema_angle[i]) / 90.0;
        F(i,16) = accel_v[i] * corr_score[i];
        F(i,17) = atr_v[i] * accel_v[i];
        F(i,18) = bb_width[i] * corr_score[i];
        F(i,19) = volr_v[i] * accel_v[i];
        F(i,20) = (close[i] / ema_fast[i]) - 1.0;
        F(i,21) = (ema_slow[i] / ema_fast[i]) - 1.0;
        F(i,22) = rsi_v[i] / 100.0 * (accel_v[i] / 45.0);
        F(i,23) = (regime_type[i] == 1 ? 1.0 : 0.0);
    }

    F.replace(arma::datum::nan, 0.0);
    return F;
}

// JSON короткий отчёт
json make_features(const std::vector<double>& o,
                   const std::vector<double>& h,
                   const std::vector<double>& l,
                   const std::vector<double>& c,
                   const std::vector<double>& v) {
    arma::Mat<double> raw(o.size(), 6, arma::fill::zeros);
    for (size_t i = 0; i < o.size(); ++i) {
        raw(i, 1) = o[i];
        raw(i, 2) = h[i];
        raw(i, 3) = l[i];
        raw(i, 4) = c[i];
        raw(i, 5) = v[i];
    }
    arma::Mat<double> F = build_feature_matrix(raw);
    json out;
    out["version"] = FEAT_VERSION;
    out["rows"] = F.n_rows;
    out["cols"] = F.n_cols;
    return out;
}

} // namespace etai
