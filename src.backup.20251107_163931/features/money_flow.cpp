#include "money_flow.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace etai {

std::vector<double> calc_mfi(const std::vector<double>& high,
                             const std::vector<double>& low,
                             const std::vector<double>& close,
                             const std::vector<double>& volume,
                             int period)
{
    const size_t n = close.size();
    std::vector<double> mfi(n, 50.0);
    if (n < static_cast<size_t>(period) + 2) return mfi;

    std::vector<double> tp(n), mf(n);
    for (size_t i = 0; i < n; ++i) {
        tp[i] = (high[i] + low[i] + close[i]) / 3.0;
        mf[i] = tp[i] * volume[i];
    }

    for (size_t i = period; i < n; ++i) {
        double pos = 0.0, neg = 0.0;
        for (size_t j = i - period + 1; j <= i; ++j) {
            if (tp[j] > tp[j - 1])       pos += mf[j];
            else if (tp[j] < tp[j - 1])  neg += mf[j];
        }
        const double ratio = (neg <= 0.0) ? 100.0 : pos / neg;
        mfi[i] = 100.0 - (100.0 / (1.0 + ratio));
    }
    return mfi;
}

std::vector<double> calc_flow_ratio(const std::vector<double>& mfi)
{
    const size_t n = mfi.size();
    std::vector<double> r(n, 0.5);
    for (size_t i = 1; i < n; ++i) {
        double x = mfi[i] / 100.0;
        if (!std::isfinite(x)) x = 0.5;
        r[i] = std::clamp(x, 0.0, 1.0);
    }
    return r;
}

std::vector<double> calc_cum_flow(const std::vector<double>& flow_ratio)
{
    const size_t n = flow_ratio.size();
    std::vector<double> cf(n, 0.0);
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double d = std::isfinite(flow_ratio[i]) ? (flow_ratio[i] - 0.5) : 0.0;
        sum += d;
        cf[i] = sum;
    }
    if (n > 1) {
        const double mean = std::accumulate(cf.begin(), cf.end(), 0.0) / static_cast<double>(n);
        double maxdev = 0.0;
        for (double v : cf) maxdev = std::max(maxdev, std::fabs(v - mean));
        const double scale = (maxdev > 0.0) ? (1.0 / maxdev) : 1.0;
        for (double &v : cf) v = (v - mean) * scale;
    }
    return cf;
}

std::vector<double> calc_sfi(const std::vector<double>& flow_ratio,
                             const std::vector<double>& mfi)
{
    const size_t n = std::min(flow_ratio.size(), mfi.size());
    std::vector<double> sfi(n, 0.0);
    for (size_t i = 0; i < n; ++i) {
        double fr = std::isfinite(flow_ratio[i]) ? flow_ratio[i] : 0.5;
        double mi = std::isfinite(mfi[i]) ? (mfi[i] / 100.0) : 0.5;
        // центрируем поток вокруг 0, мягко взвешиваем MFI:
        sfi[i] = (fr - 0.5) * 2.0 * mi; // диапазон примерно [-1..1]
    }
    return sfi;
}

} // namespace etai
