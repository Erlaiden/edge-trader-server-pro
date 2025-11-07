#include "context_detector.h"
#include <algorithm>
#include <cmath>
#include <limits>

using nlohmann::json;

namespace etai {

static inline double ctx_safe_div(double a, double b){
    if(!std::isfinite(a) || !std::isfinite(b) || std::fabs(b) < 1e-12) return 0.0;
    return a/b;
}

static inline double ctx_sma_one(const std::vector<double>& v, int period, size_t i){
    if (period <= 0 || i + 1 < (size_t)period) return std::numeric_limits<double>::quiet_NaN();
    double s = 0.0;
    for(size_t j = i + 1 - period; j <= i; ++j) s += v[j];
    return s / period;
}

static inline double ctx_atr_one(const std::vector<double>& h,
                                 const std::vector<double>& l,
                                 const std::vector<double>& c,
                                 int period, size_t i){
    if (i + 1 < (size_t)period + 1) return std::numeric_limits<double>::quiet_NaN();
    double s = 0.0;
    for(size_t j = i + 1 - period; j <= i; ++j){
        double tr = std::max({h[j] - l[j],
                              std::fabs(h[j] - c[j - 1]),
                              std::fabs(l[j] - c[j - 1])});
        s += tr;
    }
    return s / period;
}

static inline int ctx_pick_phase(double energy, double liquidity, double body_rel_atr, double body_sign){
    if(energy > 1.1 && liquidity > 0.6 && body_rel_atr > 0.25) return 1; // expansion
    if(energy > 1.0 && liquidity > 0.6 && body_rel_atr > 0.15 && std::fabs(body_sign) < 0.3) return 2; // distribution
    if(energy > 0.9 && body_sign < 0.0) return 3; // correction
    return 0; // accumulation
}

static inline double ctx_candle_sentiment(double open, double close, double atr){
    double body = close - open;
    double denom = std::max(1e-6, atr);
    return std::tanh(body / denom);
}

static inline void ctx_session_cycle(long long ts_ms, double& s, double& c){
    long long hour = (ts_ms / 1000LL / 3600LL) % 24LL;
    double ang = (2.0 * M_PI * (double)hour) / 24.0;
    s = std::sin(ang);
    c = std::cos(ang);
}

ContextSeries compute_context(const std::vector<long long>& ts_ms,
                              const std::vector<double>& open,
                              const std::vector<double>& high,
                              const std::vector<double>& low,
                              const std::vector<double>& close,
                              const std::vector<double>& volume)
{
    const size_t n = close.size();
    ContextSeries out;
    out.energy.assign(n, 0.0);
    out.liquidity.assign(n, 0.0);
    out.session_sin.assign(n, 0.0);
    out.session_cos.assign(n, 0.0);
    out.sentiment.assign(n, 0.0);
    out.phase.assign(n, 0);

    std::vector<double> atr_v(n, std::numeric_limits<double>::quiet_NaN());
    for(size_t i=0;i<n;++i){
        atr_v[i] = ctx_atr_one(high, low, close, 14, i);
    }

    for(size_t i=0;i<n;++i){
        double atr_sma = ctx_sma_one(atr_v, 14, i);
        double energy  = (std::isfinite(atr_sma) && atr_sma > 0.0) ? ctx_safe_div(atr_v[i], atr_sma) : 0.0;
        out.energy[i]  = energy;

        double vol_max = 0.0;
        if(i >= 20){
            vol_max = *std::max_element(volume.begin() + (i-20), volume.begin() + i + 1);
        }
        out.liquidity[i] = (vol_max > 0.0) ? ctx_safe_div(volume[i], vol_max) : 0.0;

        double ss, cc; ctx_session_cycle(ts_ms[i], ss, cc);
        out.session_sin[i] = ss;
        out.session_cos[i] = cc;

        double sent = ctx_candle_sentiment(open[i], close[i], std::isfinite(atr_v[i]) ? atr_v[i] : 0.0);
        if(i>0) sent = 0.7*sent + 0.3*out.sentiment[i-1];
        out.sentiment[i] = sent;

        double body_rel_atr = ctx_safe_div(std::fabs(close[i]-open[i]), std::max(1e-6, atr_v[i]));
        double body_sign    = ctx_safe_div((close[i] - open[i]), std::max(1e-6, atr_v[i]));
        out.phase[i] = ctx_pick_phase(energy, out.liquidity[i], body_rel_atr, body_sign);
    }

    return out;
}

nlohmann::json context_tail_to_json(const std::vector<long long>& ts_ms,
                                    const std::vector<double>& open,
                                    const std::vector<double>& high,
                                    const std::vector<double>& low,
                                    const std::vector<double>& close,
                                    const std::vector<double>& volume)
{
    ContextSeries s = compute_context(ts_ms, open, high, low, close, volume);
    const size_t n = close.size();
    nlohmann::json j;
    j["rows"]          = n;
    j["energy_last"]    = n? s.energy.back()    : 0.0;
    j["liquidity_last"] = n? s.liquidity.back() : 0.0;
    j["session_sin"]    = n? s.session_sin.back(): 0.0;
    j["session_cos"]    = n? s.session_cos.back(): 0.0;
    j["sentiment_last"] = n? s.sentiment.back() : 0.0;
    j["phase_last"]     = n? s.phase.back()     : 0;
    return j;
}

} // namespace etai
