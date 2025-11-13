#include "regime_detector.h"
#include <cmath>
#include <algorithm>

namespace etai {

// ------------------------------------------------------------
// Вспомогательная функция для чтения HTF score из JSON
// ------------------------------------------------------------
static double get_htf_score(const nlohmann::json& htf, const char* key) {
    if (!htf.contains(key) || !htf[key].is_object()) return 0.0;
    const auto& h = htf[key];
    if (!h.contains("score")) return 0.0;
    auto score_val = h["score"];
    if (score_val.is_null()) return 0.0;
    return score_val.is_number() ? score_val.get<double>() : 0.0;
}

// ------------------------------------------------------------
// Корректное безопасное извлечение OHLCV из raw-матрицы
// raw: rows = candles, cols = [ts, open, high, low, close, volume]
// ------------------------------------------------------------
static inline double r_open (const arma::mat& M, size_t i){ return M(i,1); }
static inline double r_high (const arma::mat& M, size_t i){ return M(i,2); }
static inline double r_low  (const arma::mat& M, size_t i){ return M(i,3); }
static inline double r_close(const arma::mat& M, size_t i){ return M(i,4); }
static inline double r_vol  (const arma::mat& M, size_t i){ return M(i,5); }

// ------------------------------------------------------------
// Детекция манипуляции (корректная форма данных)
// ------------------------------------------------------------
bool is_manipulation(const arma::mat& M15) {
    size_t N = M15.n_rows;
    if (N < 5 || M15.n_cols < 6) return false;

    double open  = r_open (M15, N-1);
    double high  = r_high (M15, N-1);
    double low   = r_low  (M15, N-1);
    double close = r_close(M15, N-1);

    double full_range = high - low;
    if (full_range < 1e-9) return false;

    double upper_wick = high - std::max(open, close);
    double lower_wick = std::min(open, close) - low;

    // длинная тень → возможная манипуляция
    if (upper_wick/full_range > 0.7 || lower_wick/full_range > 0.7) {
        if (N >= 2) {
            double prev_close = r_close(M15, N-2);
            if (prev_close > 0.0) {
                double change = (close - prev_close)/prev_close;
                if (std::fabs(change) > 0.03) return true;
            }
        }
    }
    return false;
}

// ------------------------------------------------------------
// Детекция пробоя
// ------------------------------------------------------------
bool is_breakout(const arma::mat& M15, double atr, double avg_atr) {
    size_t N = M15.n_rows;
    if (N < 5 || M15.n_cols < 6) return false;

    if (atr < avg_atr * 1.5) return false;

    double c0 = r_close(M15, N-1);
    double c1 = r_close(M15, N-2);
    double c2 = r_close(M15, N-3);

    double change_recent = std::fabs((c0 - c1) / c1);
    double change_prev   = std::fabs((c1 - c2) / c2);

    return (change_recent > change_prev * 1.5 && change_recent > 0.02);
}

// ------------------------------------------------------------
// Детекция флэта
// ------------------------------------------------------------
bool is_ranging(const arma::mat& M15, double atr, double avg_atr) {
    size_t N = M15.n_rows;
    if (N < 20 || M15.n_cols < 6) return false;

    if (atr > avg_atr * 0.8) return false;

    double sum = 0.0;
    for (size_t i = N-20; i < N; ++i)
        sum += r_close(M15, i);

    double ma20 = sum / 20.0;
    double oldest = r_close(M15, N-20);
    double ma_change = (ma20 - oldest) / oldest;

    return std::fabs(ma_change) < 0.01;
}

// ------------------------------------------------------------
// Позиция цены внутри канала MA20±2ATR
// ------------------------------------------------------------
double get_channel_position(const arma::mat& M15, double atr) {
    size_t N = M15.n_rows;
    if (N < 20 || M15.n_cols < 6) return 0.5;

    double sum = 0.0;
    for (size_t i = N-20; i < N; ++i)
        sum += r_close(M15, i);

    double ma20 = sum / 20.0;
    double current = r_close(M15, N-1);

    double upper = ma20 + 2.0 * atr;
    double lower = ma20 - 2.0 * atr;
    if (upper <= lower) return 0.5;

    double pos = (current - lower) / (upper - lower);
    return std::max(0.0, std::min(1.0, pos));
}

// ------------------------------------------------------------
// Детекция рыночного режима
// Упрощённая версия: 15m + HTF (60, 240)
// ------------------------------------------------------------
MarketRegime detect_regime(
    const nlohmann::json& htf,
    const arma::mat& M15,
    double atr,
    double avg_atr
) {
    size_t N = M15.n_rows;
    if (N < 5 || M15.n_cols < 6)
        return MarketRegime::UNCERTAIN;

    // 1. Манипуляция
    if (is_manipulation(M15))
        return MarketRegime::MANIPULATION;

    // 2. HTF signals
    double htf_60  = get_htf_score(htf, "60");   // 1H
    double htf_240 = get_htf_score(htf, "240");  // 4H

    bool h1_up    = htf_60  >  0.3;
    bool h1_down  = htf_60  < -0.3;
    bool h4_up    = htf_240 >  0.3;
    bool h4_down  = htf_240 < -0.3;

    // 3. Пробой
    if (is_breakout(M15, atr, avg_atr)) {
        double c0 = r_close(M15, N-1);
        double c1 = r_close(M15, N-2);
        double change = (c0 - c1) / c1;
        if (change >  0.01) return MarketRegime::BREAKOUT_UP;
        if (change < -0.01) return MarketRegime::BREAKOUT_DOWN;
    }

    // 4. Сильные тренды
    if (h4_up && h1_up)   return MarketRegime::STRONG_UPTREND;
    if (h4_down && h1_down) return MarketRegime::STRONG_DOWNTREND;

    // 5. Коррекции
    if (h4_up && h1_down)   return MarketRegime::CORRECTION_UP;
    if (h4_down && h1_up)   return MarketRegime::CORRECTION_DOWN;

    // 6. Флет
    if (is_ranging(M15, atr, avg_atr))
        return MarketRegime::RANGE_BOUND;

    // 7. Если есть хоть какое-то направление 4H — торгуем в его сторону
    if (h4_up)   return MarketRegime::STRONG_UPTREND;
    if (h4_down) return MarketRegime::STRONG_DOWNTREND;

    // 8. Совсем непонятно
    return MarketRegime::UNCERTAIN;
}

// ------------------------------------------------------------
// Параметры режима
// ------------------------------------------------------------
RegimeParams get_regime_params(MarketRegime regime) {
    switch (regime) {
        case MarketRegime::STRONG_UPTREND:
            return {true,  false, 0.04,  0.015, 22.0, "Strong uptrend - only LONG",  false};
        case MarketRegime::STRONG_DOWNTREND:
            return {false, true,  0.04,  0.015, 22.0, "Strong downtrend - only SHORT", false};
        case MarketRegime::RANGE_BOUND:
            return {true,  true,  0.015, 0.008, 20.0, "Range - mean reversion",       true};
        case MarketRegime::CORRECTION_UP:
            return {true,  false, 0.035, 0.02,  22.0, "Correction in uptrend - LONG reversal",  false};
        case MarketRegime::CORRECTION_DOWN:
            return {false, true,  0.035, 0.02,  22.0, "Correction in downtrend - SHORT reversal", false};
        case MarketRegime::BREAKOUT_UP:
            return {true,  false, 0.08,  0.025, 20.0, "Breakout up - LONG immediately",  false};
        case MarketRegime::BREAKOUT_DOWN:
            return {false, true,  0.08,  0.025, 20.0, "Breakout down - SHORT immediately", false};
        case MarketRegime::MANIPULATION:
            return {false, false, 0.0,   0.0,  100.0, "Manipulation - do not trade", false};
        case MarketRegime::UNCERTAIN:
        default:
            return {false, false, 0.0,   0.0,  100.0, "Uncertain - do not trade", false};
    }
}

std::string regime_to_string(MarketRegime regime) {
    switch (regime) {
        case MarketRegime::STRONG_UPTREND:  return "STRONG_UPTREND";
        case MarketRegime::STRONG_DOWNTREND:return "STRONG_DOWNTREND";
        case MarketRegime::RANGE_BOUND:     return "RANGE_BOUND";
        case MarketRegime::CORRECTION_UP:   return "CORRECTION_UP";
        case MarketRegime::CORRECTION_DOWN: return "CORRECTION_DOWN";
        case MarketRegime::BREAKOUT_UP:     return "BREAKOUT_UP";
        case MarketRegime::BREAKOUT_DOWN:   return "BREAKOUT_DOWN";
        case MarketRegime::MANIPULATION:    return "MANIPULATION";
        case MarketRegime::UNCERTAIN:       return "UNCERTAIN";
        default:                            return "UNKNOWN";
    }
}

} // namespace etai
