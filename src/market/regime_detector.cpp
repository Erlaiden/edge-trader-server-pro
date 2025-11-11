#include "regime_detector.h"
#include <cmath>
#include <algorithm>

namespace etai {

// Вспомогательная функция для извлечения HTF score
static double get_htf_score(const nlohmann::json& htf, const char* key) {
    if (!htf.contains(key) || !htf[key].is_object()) return 0.0;
    const auto& h = htf[key];
    if (!h.contains("score")) return 0.0;
    auto score_val = h["score"];
    if (score_val.is_null()) return 0.0;  // ✅ Обработка null
    return score_val.is_number() ? score_val.get<double>() : 0.0;
}

// Проверка на манипуляцию
bool is_manipulation(const arma::mat& M15) {
    if (M15.n_cols < 2 || M15.n_rows < 5) return false;

    size_t N = M15.n_cols;
    double open = M15(1, N-1);
    double high = M15(2, N-1);
    double low = M15(3, N-1);
    double close = M15(4, N-1);

    double body = std::fabs(close - open);
    double full_range = high - low;

    if (full_range < 1e-9) return false;

    double upper_wick = high - std::max(open, close);
    double lower_wick = std::min(open, close) - low;

    if (upper_wick / full_range > 0.7 || lower_wick / full_range > 0.7) {
        if (N >= 2) {
            double prev_close = M15(4, N-2);
            double change = (close - prev_close) / prev_close;
            if (std::fabs(change) > 0.03) {
                return true;
            }
        }
    }

    return false;
}

// Проверка на пробой
bool is_breakout(const arma::mat& M15, double atr, double avg_atr) {
    if (M15.n_cols < 5 || M15.n_rows < 5) return false;

    size_t N = M15.n_cols;

    if (atr < avg_atr * 1.5) return false;

    double c0 = M15(4, N-1);
    double c1 = M15(4, N-2);
    double c2 = M15(4, N-3);

    double change_recent = std::fabs((c0 - c1) / c1);
    double change_prev = std::fabs((c1 - c2) / c2);

    if (change_recent > change_prev * 1.5 && change_recent > 0.02) {
        return true;
    }

    return false;
}

// Проверка на флет
bool is_ranging(const arma::mat& M15, double atr, double avg_atr) {
    if (M15.n_cols < 20 || M15.n_rows < 5) return false;

    if (atr > avg_atr * 0.8) return false;

    size_t N = M15.n_cols;

    double sum = 0.0;
    for (size_t i = N-20; i < N; ++i) {
        sum += M15(4, i);
    }
    double ma20 = sum / 20.0;

    double oldest = M15(4, N-20);
    double ma_change = (ma20 - oldest) / oldest;

    if (std::fabs(ma_change) < 0.01) {
        return true;
    }

    return false;
}

// Позиция цены в канале (0.0 = низ, 1.0 = верх)
double get_channel_position(const arma::mat& M15, double atr) {
    if (M15.n_cols < 20 || M15.n_rows < 5) return 0.5;

    size_t N = M15.n_cols;

    double sum = 0.0;
    for (size_t i = N-20; i < N; ++i) {
        sum += M15(4, i);
    }
    double ma20 = sum / 20.0;

    double current = M15(4, N-1);

    double upper = ma20 + 2.0 * atr;
    double lower = ma20 - 2.0 * atr;

    if (upper <= lower) return 0.5;

    double position = (current - lower) / (upper - lower);
    return std::max(0.0, std::min(1.0, position));
}

// ✅ УПРОЩЕННАЯ ВЕРСИЯ: Работает БЕЗ дневок, только 4H + 1H
MarketRegime detect_regime(
    const nlohmann::json& htf,
    const arma::mat& M15,
    double atr,
    double avg_atr
) {
    // 1. Проверка на манипуляцию
    if (is_manipulation(M15)) {
        return MarketRegime::MANIPULATION;
    }

    // 2. Извлекаем HTF scores (БЕЗ дневок!)
    double htf_240 = get_htf_score(htf, "240");  // 4H
    double htf_60 = get_htf_score(htf, "60");    // 1H

    bool h4_up = htf_240 > 0.3;      // ✅ Снизили порог с 0.5 до 0.3
    bool h4_down = htf_240 < -0.3;
    bool h1_up = htf_60 > 0.3;
    bool h1_down = htf_60 < -0.3;

    // 3. Проверка на пробой
    if (is_breakout(M15, atr, avg_atr)) {
        if (M15.n_cols >= 2) {
            size_t N = M15.n_cols;
            double change = (M15(4, N-1) - M15(4, N-2)) / M15(4, N-2);
            if (change > 0.01) return MarketRegime::BREAKOUT_UP;
            if (change < -0.01) return MarketRegime::BREAKOUT_DOWN;
        }
    }

    // 4. Сильные тренды (4H + 1H согласны)
    if (h4_up && h1_up) {
        return MarketRegime::STRONG_UPTREND;
    }
    if (h4_down && h1_down) {
        return MarketRegime::STRONG_DOWNTREND;
    }

    // 5. Коррекции (4H и 1H расходятся)
    if (h4_up && h1_down) {
        return MarketRegime::CORRECTION_UP;
    }
    if (h4_down && h1_up) {
        return MarketRegime::CORRECTION_DOWN;
    }

    // 6. Флет
    if (is_ranging(M15, atr, avg_atr)) {
        return MarketRegime::RANGE_BOUND;
    }

    // 7. ✅ ИЗМЕНЕНИЕ: Если есть хоть какое-то направление на 4H - торгуем
    if (h4_up) {
        return MarketRegime::STRONG_UPTREND;  // Предпочитаем LONG
    }
    if (h4_down) {
        return MarketRegime::STRONG_DOWNTREND;  // Предпочитаем SHORT
    }

    // 8. Только если совсем непонятно - не торгуем
    return MarketRegime::UNCERTAIN;
}

// ✅ СНИЗИЛИ пороги min_confidence с 60% до 40%
RegimeParams get_regime_params(MarketRegime regime) {
    switch (regime) {
        case MarketRegime::STRONG_UPTREND:
            return {true, false, 0.04, 0.015, 22.0, "Strong uptrend - only LONG", false};

        case MarketRegime::STRONG_DOWNTREND:
            return {false, true, 0.04, 0.015, 22.0, "Strong downtrend - only SHORT", false};

        case MarketRegime::RANGE_BOUND:
            return {true, true, 0.015, 0.008, 20.0, "Range - mean reversion", true};

        case MarketRegime::CORRECTION_UP:
            return {true, false, 0.035, 0.02, 22.0, "Correction in uptrend - LONG reversal", false};

        case MarketRegime::CORRECTION_DOWN:
            return {false, true, 0.035, 0.02, 22.0, "Correction in downtrend - SHORT reversal", false};

        case MarketRegime::BREAKOUT_UP:
            return {true, false, 0.08, 0.025, 20.0, "Breakout up - LONG immediately", false};

        case MarketRegime::BREAKOUT_DOWN:
            return {false, true, 0.08, 0.025, 20.0, "Breakout down - SHORT immediately", false};

        case MarketRegime::MANIPULATION:
            return {false, false, 0.0, 0.0, 100.0, "Manipulation - do not trade", false};

        case MarketRegime::UNCERTAIN:
        default:
            return {false, false, 0.0, 0.0, 100.0, "Uncertain - do not trade", false};
    }
}

std::string regime_to_string(MarketRegime regime) {
    switch (regime) {
        case MarketRegime::STRONG_UPTREND: return "STRONG_UPTREND";
        case MarketRegime::STRONG_DOWNTREND: return "STRONG_DOWNTREND";
        case MarketRegime::RANGE_BOUND: return "RANGE_BOUND";
        case MarketRegime::CORRECTION_UP: return "CORRECTION_UP";
        case MarketRegime::CORRECTION_DOWN: return "CORRECTION_DOWN";
        case MarketRegime::BREAKOUT_UP: return "BREAKOUT_UP";
        case MarketRegime::BREAKOUT_DOWN: return "BREAKOUT_DOWN";
        case MarketRegime::MANIPULATION: return "MANIPULATION";
        case MarketRegime::UNCERTAIN: return "UNCERTAIN";
        default: return "UNKNOWN";
    }
}

} // namespace etai
