#include "features.h"
#include "context_detector.h"
#include "json.hpp"
#include <armadillo>
#include <cmath>
#include <algorithm>
#include <vector>
#include <cstdlib>   // getenv

// ВРЕМЕННО до правки CMakeLists.txt — подтягиваем реализацию контекста сюда:
#include "context_detector.cpp"

// Money Flow layer
#include "money_flow.h"

using json = nlohmann::json;
using namespace arma;

namespace etai {

// Версия фич по умолчанию (без Money Flow)
constexpr int FEAT_VERSION_BASE = 9;

static inline bool env_enabled(const char* k){
    const char* s = std::getenv(k);
    if(!s || !*s) return false;
    // "1", "true", "yes" включают
    return (s[0]=='1') || (s[0]=='T'||s[0]=='t') || (s[0]=='Y'||s[0]=='y');
}

// ---------- вспомогательные функции ----------
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
    for (size_t j = i + 1 - period; j <= i; ++j) s += v[j];
    return s / period;
}

static double rsi_one(const std::vector<double>& c, int period, size_t i) {
    if (i + 1 < (size_t)period + 1) return NAN;
    double gain = 0, loss = 0;
    for (size_t j = i + 1 - period; j <= i; ++j) {
        double diff = c[j] - c[j - 1];
        if (diff >= 0) gain += diff; else loss -= diff;
    }
    if (loss == 0) return 100.0;
    double rs = gain / loss;
    return 100.0 - (100.0 / (1.0 + rs));
}

static double atr_one(const std::vector<double>& h,
                      const std::vector<double>& l,
                      const std::vector<double>& c,
                      int period, size_t i)
{
    if (i + 1 < (size_t)period + 1) return NAN;
    double s = 0;
    for (size_t j = i + 1 - period; j <= i; ++j) {
        double tr = std::max({h[j] - l[j],
                              std::fabs(h[j] - c[j - 1]),
                              std::fabs(l[j] - c[j - 1])});
        s += tr;
    }
    return s / period;
}

// ---------- построение матрицы признаков ----------
arma::Mat<double> build_feature_matrix(const arma::Mat<double>& raw) {
    if (raw.n_rows < 30 || raw.n_cols < 6) return arma::Mat<double>();

    const bool ENABLE_MFLOW = env_enabled("ETAI_FEAT_ENABLE_MFLOW");

    size_t n = raw.n_rows;
    std::vector<long long> ts(n);
    std::vector<double> open(n), high(n), low(n), close(n), vol(n);
    for (size_t i = 0; i < n; ++i) {
        ts[i]    = static_cast<long long>(raw(i, 0));
        open[i]  = raw(i, 1);
        high[i]  = raw(i, 2);
        low[i]   = raw(i, 3);
        close[i] = raw(i, 4);
        vol[i]   = raw(i, 5);
    }

    std::vector<double> ema_fast(n), ema_slow(n), rsi_v(n),
                        macd_v(n), macd_hist(n), atr_v(n), accel_v(n), slope_v(n);

    for (size_t i = 0; i < n; ++i) {
        ema_fast[i] = ema_one(close, 12, i);
        ema_slow[i] = ema_one(close, 26, i);
        rsi_v[i]    = rsi_one(close, 14, i);
        atr_v[i]    = atr_one(high, low, close, 14, i);

        // ускорение и наклон
        if (i >= 2) {
            double d1 = close[i] - close[i - 1];
            double d2 = close[i - 1] - close[i - 2];
            accel_v[i] = d1 - d2;
            slope_v[i] = (i >= 3) ? (close[i] - close[i - 3]) : 0.0;
        } else {
            accel_v[i] = 0.0;
            slope_v[i] = 0.0;
        }
    }

    // MACD
    for (size_t i = 0; i < n; ++i) {
        macd_v[i] = ema_fast[i] - ema_slow[i];
        double macd_signal = ema_one(macd_v, 9, i);
        macd_hist[i] = macd_v[i] - macd_signal;
    }

    // контекст
    ContextSeries ctx = compute_context(ts, open, high, low, close, vol);

    // Money Flow (необязательный блок)
    std::vector<double> mfi, flow_ratio, cum_flow, sfi;
    if (ENABLE_MFLOW) {
        mfi        = calc_mfi(high, low, close, vol, 14);
        flow_ratio = calc_flow_ratio(mfi);
        cum_flow   = calc_cum_flow(flow_ratio);
        sfi        = calc_sfi(flow_ratio, mfi);
    }

    // Матрица признаков
    const size_t D_base = 28;
    const size_t D_mflow = ENABLE_MFLOW ? 4 : 0;
    const size_t D = D_base + D_mflow;

    arma::Mat<double> F(n, D, arma::fill::zeros);

    for (size_t i = 0; i < n; ++i) {
        // базовые технические
        F(i, 0) = ema_fast[i] - ema_slow[i];   // trend spread
        F(i, 1) = rsi_v[i] / 100.0;            // RSI
        F(i, 2) = macd_v[i];
        F(i, 3) = macd_hist[i];
        F(i, 4) = atr_v[i];
        F(i, 5) = accel_v[i];
        F(i, 6) = slope_v[i];

        // контекст
        F(i, 7)  = ctx.energy[i];
        F(i, 8)  = ctx.liquidity[i];
        F(i, 9)  = ctx.sentiment[i];
        F(i, 10) = ctx.session_sin[i];
        F(i, 11) = ctx.session_cos[i];

        // one-hot фазы
        int ph = ctx.phase[i];
        F(i, 12) = (ph == 1) ? 1.0 : 0.0; // expansion
        F(i, 13) = (ph == 2) ? 1.0 : 0.0; // distribution
        F(i, 14) = (ph == 3) ? 1.0 : 0.0; // correction

        // производные
        double diff = close[i] - open[i];
        F(i, 15) = (open[i] > 0) ? diff / open[i] : 0.0; // дневной %
        F(i, 16) = (i > 0) ? close[i] - close[i - 1] : 0.0;
        F(i, 17) = (i > 0) ? (vol[i] - vol[i - 1]) : 0.0;
        F(i, 18) = (i >= 5)  ? sma_one(close, 5, i) - sma_one(close, 10, i) : 0.0;
        F(i, 19) = (i >= 14) ? (rsi_one(close, 14, i) - 50.0) / 50.0 : 0.0;
        F(i, 20) = std::fabs(ema_fast[i] - ema_slow[i]) / (atr_v[i] + 1e-8);
        F(i, 21) = (i >= 10) ? sma_one(vol, 10, i) / (sma_one(vol, 20, i) + 1e-8) : 0.0;
        F(i, 22) = (i >= 20) ? sma_one(atr_v, 10, i) / (sma_one(atr_v, 20, i) + 1e-8) : 0.0;
        F(i, 23) = (macd_v[i] > 0 && rsi_v[i] > 50) ? 1.0 : 0.0;
        F(i, 24) = (macd_v[i] < 0 && rsi_v[i] < 50) ? 1.0 : 0.0;
        F(i, 25) = ctx.energy[i] * (macd_v[i] > 0 ? 1 : -1);
        F(i, 26) = ctx.sentiment[i] * (ctx.energy[i]);
        F(i, 27) = (ctx.phase[i] == 3 && ctx.sentiment[i] < 0) ? 1.0 : 0.0;

        // --- Money Flow block (опционально) ---
        if (ENABLE_MFLOW) {
            size_t k = D_base;
            // Нормировки: MFI -> [0..1], cum_flow/sfi уже ~[-1..1]
            double mfi01 = (i < mfi.size() && std::isfinite(mfi[i])) ? (mfi[i] / 100.0) : 0.5;
            double fr    = (i < flow_ratio.size() && std::isfinite(flow_ratio[i])) ? flow_ratio[i] : 0.5;
            double cf    = (i < cum_flow.size()   && std::isfinite(cum_flow[i]))   ? cum_flow[i]   : 0.0;
            double sfi_v = (i < sfi.size()        && std::isfinite(sfi[i]))        ? sfi[i]        : 0.0;
            F(i, k+0) = mfi01;
            F(i, k+1) = fr;
            F(i, k+2) = cf;
            F(i, k+3) = sfi_v;
        }
    }

    F.replace(arma::datum::nan, 0.0);
    return F;
}

// ---------- JSON экспортер ----------
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
    // Версию поднимаем до 10 только если включен Money Flow
    out["version"] = env_enabled("ETAI_FEAT_ENABLE_MFLOW") ? 10 : FEAT_VERSION_BASE;
    out["rows"] = F.n_rows;
    out["cols"] = F.n_cols;
    return out;
}

} // namespace etai
