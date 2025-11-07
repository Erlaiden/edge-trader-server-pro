#include "money_flow.h"
#include <armadillo>
#include <cmath>
#include <vector>
#include <algorithm>
#include <limits>

using json = nlohmann::json;
using namespace arma;

namespace etai {

// ВНИМАНИЕ: FlowMetrics объявлён в money_flow.h — здесь НЕ дублируем.

// Локальный helper с уникальным именем, чтобы не конфликтовать с features.cpp
static inline double mf_sma_one(const std::vector<double>& v, int period, size_t i) {
    if (period <= 0) return std::numeric_limits<double>::quiet_NaN();
    if (i + 1 < (size_t)period) return std::numeric_limits<double>::quiet_NaN();
    double s = 0.0;
    for (size_t j = i + 1 - period; j <= i; ++j) s += v[j];
    return s / period;
}

FlowMetrics compute_money_flow(const std::vector<double>& open,
                               const std::vector<double>& high,
                               const std::vector<double>& low,
                               const std::vector<double>& close,
                               const std::vector<double>& volume)
{
    const size_t n = close.size();
    FlowMetrics f;
    f.mfi.assign(n, std::numeric_limits<double>::quiet_NaN());
    f.flow_ratio.assign(n, std::numeric_limits<double>::quiet_NaN());
    f.cum_flow.assign(n, std::numeric_limits<double>::quiet_NaN());
    f.sfi.assign(n, std::numeric_limits<double>::quiet_NaN());

    std::vector<double> pos_mf(n, 0.0), neg_mf(n, 0.0);
    std::vector<double> energy(n, 0.0);

    for (size_t i = 1; i < n; ++i) {
        const double typical      = (high[i] + low[i] + close[i]) / 3.0;
        const double prev_typical = (high[i-1] + low[i-1] + close[i-1]) / 3.0;
        const double mf = typical * volume[i];

        if (typical > prev_typical) pos_mf[i] = mf; else neg_mf[i] = mf;

        // MFI
        const double denom = (neg_mf[i] == 0.0) ? std::numeric_limits<double>::quiet_NaN()
                                                : (pos_mf[i] / neg_mf[i]);
        f.mfi[i] = 100.0 - (100.0 / (1.0 + denom));

        // FlowRatio
        f.flow_ratio[i] = (volume[i-1] > 0.0)
            ? (close[i] - open[i]) * volume[i] / volume[i-1]
            : std::numeric_limits<double>::quiet_NaN();

        // CumFlow
        const double prev_cf = (i>0 && std::isfinite(f.cum_flow[i-1])) ? f.cum_flow[i-1] : 0.0;
        f.cum_flow[i] = prev_cf + (std::isfinite(f.flow_ratio[i]) ? f.flow_ratio[i] : 0.0);

        // Energy ~ объём относительно своей SMA
        const double mean_vol = mf_sma_one(volume, 14, i);
        energy[i] = (std::isfinite(mean_vol) && mean_vol > 0.0) ? volume[i] / mean_vol : 0.0;

        // Smart Flow Index (нормированный)
        const double mfi_norm = std::isfinite(f.mfi[i]) ? (f.mfi[i] / 100.0) : 0.0;
        const double vol_ratio = (std::isfinite(mean_vol) && mean_vol > 0.0) ? volume[i] / mean_vol : 0.0;

        // Базовая линейная композиция (веса можно калибровать позже)
        f.sfi[i] = 0.5 * mfi_norm + 0.3 * energy[i] + 0.2 * vol_ratio;
    }

    return f;
}

json money_flow_to_json(const std::vector<double>& open,
                        const std::vector<double>& high,
                        const std::vector<double>& low,
                        const std::vector<double>& close,
                        const std::vector<double>& volume)
{
    FlowMetrics f = compute_money_flow(open, high, low, close, volume);
    json j;
    j["rows"] = close.size();
    j["mfi_last"] = f.mfi.empty() ? 0.0 : f.mfi.back();
    j["flow_ratio_last"] = f.flow_ratio.empty() ? 0.0 : f.flow_ratio.back();
    j["cum_flow_last"] = f.cum_flow.empty() ? 0.0 : f.cum_flow.back();
    j["sfi_last"] = f.sfi.empty() ? 0.0 : f.sfi.back();
    return j;
}

} // namespace etai
