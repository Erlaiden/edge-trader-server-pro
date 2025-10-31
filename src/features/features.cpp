#include "features.h"
#include "money_flow.h"
#include "json.hpp"
#include <armadillo>
#include <cmath>
#include <algorithm>
#include <vector>

// Временный хак до правки CMakeLists.txt:
// подтягиваем реализацию в этот TU, чтобы не ловить undefined reference.
#include "money_flow.cpp"

using json = nlohmann::json;
using namespace arma;

namespace etai {

constexpr int FEAT_VERSION = 8;

// ------------------ вспомогательные функции ------------------
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
        if (diff >= 0) gain += diff; else loss -= diff;
    }
    if (loss == 0) return 100.0;
    double rs = gain / loss;
    return 100.0 - (100.0 / (1.0 + rs));
}

static double atr_one(const std::vector<double>& h,
                      const std::vector<double>& l,
                      const std::vector<double>& c,
                      int period, size_t i) {
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

// ------------------ построение фич ------------------
arma::Mat<double> build_feature_matrix(const arma::Mat<double>& raw) {
    if (raw.n_rows < 50 || raw.n_cols < 6) return arma::Mat<double>();

    size_t n = raw.n_rows;
    std::vector<double> open(n), high(n), low(n), close(n), vol(n);
    for (size_t i = 0; i < n; ++i) {
        open[i]  = raw(i, 1);
        high[i]  = raw(i, 2);
        low[i]   = raw(i, 3);
        close[i] = raw(i, 4);
        vol[i]   = raw(i, 5);
    }

    std::vector<double> ema_fast(n), ema_slow(n), rsi_v(n), atr_v(n),
                        macd_v(n), macd_signal(n), macd_hist(n),
                        bb_width(n), volr_v(n),
                        corr_v(n), accel_v(n), slope_v(n),
                        sr_dist(n), energy_v(n), liq_v(n),
                        trap_idx(n), vp_ratio(n), manip_flag(n);

    // --- Технические и режимные признаки ---
    for (size_t i = 0; i < n; ++i) {
        ema_fast[i] = ema_one(close, 12, i);
        ema_slow[i] = ema_one(close, 26, i);
        rsi_v[i]    = rsi_one(close, 14, i);
        atr_v[i]    = atr_one(high, low, close, 14, i);

        macd_v[i] = ema_fast[i] - ema_slow[i];
        macd_signal[i] = ema_one(macd_v, 9, i);
        macd_hist[i]   = macd_v[i] - macd_signal[i];

        if (i >= 20) {
            double sma20 = sma_one(close, 20, i);
            double sd = 0;
            for (size_t j = i + 1 - 20; j <= i; ++j)
                sd += std::pow(close[j] - sma20, 2);
            sd = std::sqrt(sd / 20);
            bb_width[i] = (sma20 != 0) ? (4.0 * sd / sma20) : NAN;
        } else bb_width[i] = NAN;

        volr_v[i] = (i >= 20 && sma_one(vol, 20, i) > 0)
                    ? vol[i] / sma_one(vol, 20, i)
                    : NAN;

        // Коррекция к EMA по ATR
        corr_v[i] = (atr_v[i] > 0) ? (close[i] - ema_fast[i]) / atr_v[i] : NAN;

        // Ускорение цены
        if (i >= 2)
            accel_v[i] = (close[i] - 2 * close[i-1] + close[i-2]) / std::max(1e-12, std::fabs(close[i-2]));
        else accel_v[i] = NAN;

        // Наклон EMA
        slope_v[i] = (i >= 3) ? (ema_fast[i] - ema_fast[i-3]) / 3.0 : NAN;

        // Поддержка/сопротивление — нормированная дистанция
        double s_min = *std::min_element(low.begin() + std::max<int>(0, i-20), low.begin() + i+1);
        double s_max = *std::max_element(high.begin() + std::max<int>(0, i-20), high.begin() + i+1);
        sr_dist[i] = (s_max - close[i]) / (s_max - s_min + 1e-9);

        // Энергия и ликвидность
        double atr_sma = sma_one(atr_v, 14, i);
        energy_v[i] = (atr_sma > 0) ? atr_v[i] / atr_sma : NAN;

        double vol_max = 0;
        if (i >= 20)
            vol_max = *std::max_element(vol.begin() + i - 20, vol.begin() + i + 1);
        liq_v[i] = (vol_max > 0) ? vol[i] / vol_max : NAN;

        // Анти-манип признаки
        trap_idx[i] = (atr_v[i] > 0) ? (high[i]-low[i])/atr_v[i] - std::fabs(close[i]-open[i])/atr_v[i] : NAN;
        vp_ratio[i] = (sma_one(vol,20,i) > 0) ? std::fabs(close[i]-open[i]) / (vol[i]/(sma_one(vol,20,i)+1e-9)) : NAN;
        manip_flag[i] = (trap_idx[i] > 2.0 || vp_ratio[i] > 2.0) ? 1.0 : 0.0;
    }

    // --- Денежные потоки (MFI / Flow / SFI) через money_flow.cpp ---
    FlowMetrics mf = compute_money_flow(open, high, low, close, vol);

    // --- Сборка матрицы признаков ---
    const size_t D = 28;
    arma::Mat<double> F(n, D, arma::fill::zeros);

    for (size_t i = 0; i < n; ++i) {
        F(i,0)=ema_fast[i]-ema_slow[i];     // trend spread
        F(i,1)=rsi_v[i]/100.0;              // норм. RSI
        F(i,2)=macd_v[i];
        F(i,3)=macd_hist[i];
        F(i,4)=atr_v[i];
        F(i,5)=bb_width[i];
        F(i,6)=volr_v[i];
        F(i,7)=(close[i]-open[i]) / std::max(1e-12, open[i]); // дневной % change
        F(i,8)=corr_v[i];
        F(i,9)=accel_v[i];
        F(i,10)=slope_v[i];
        F(i,11)=sr_dist[i];
        F(i,12)=energy_v[i];
        F(i,13)=liq_v[i];
        F(i,14)=trap_idx[i];
        F(i,15)=vp_ratio[i];
        F(i,16)=manip_flag[i];
        F(i,17)=(std::isfinite(mf.mfi[i]) ? mf.mfi[i]/100.0 : NAN);
        F(i,18)=mf.flow_ratio[i];
        F(i,19)=mf.cum_flow[i];
        F(i,20)=mf.sfi[i];
        // производные
        F(i,21)=(ema_fast[i]>0)?(ema_slow[i]/ema_fast[i]):0;
        F(i,22)=std::tanh(F(i,8)*F(i,12));     // corr × energy
        F(i,23)=std::tanh(F(i,17)*F(i,20));    // MFI × SFI
        F(i,24)=energy_v[i]*liq_v[i];
        F(i,25)=std::fabs(corr_v[i])*energy_v[i];
        F(i,26)=trap_idx[i]*vp_ratio[i];
        F(i,27)=std::tanh(F(i,12)*F(i,20));    // energy × SFI
    }

    F.replace(arma::datum::nan, 0.0);
    return F;
}

// ------------------ make_features для JSON-экспорта ------------------
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
