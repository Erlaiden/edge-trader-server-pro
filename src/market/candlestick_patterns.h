#pragma once
#include <armadillo>
#include <string>
#include <cmath>

namespace etai {

enum class CandlePattern {
    NONE,
    HAMMER,              // Бычий разворот
    INVERTED_HAMMER,     // Бычий разворот (слабее)
    SHOOTING_STAR,       // Медвежий разворот
    HANGING_MAN,         // Медвежий разворот (слабее)
    ENGULFING_BULL,      // Сильный бычий
    ENGULFING_BEAR,      // Сильный медвежий
    DOJI,                // Неопределенность
    MORNING_STAR,        // Сильный бычий разворот (3 свечи)
    EVENING_STAR,        // Сильный медвежий разворот (3 свечи)
    MARUBOZU_BULL,       // Сильный бычий импульс
    MARUBOZU_BEAR        // Сильный медвежий импульс
};

struct CandleSignal {
    CandlePattern pattern;
    std::string pattern_name;
    std::string signal;          // "bullish", "bearish", "neutral", "reversal"
    double strength;             // 0.0 - 1.0
    double confidence_boost;     // Сколько добавить к confidence
    bool is_reversal;            // Разворотный паттерн?
};

// Определение одиночной свечи
inline CandleSignal detect_single_candle(const arma::mat& M) {
    CandleSignal cs;
    cs.pattern = CandlePattern::NONE;
    cs.confidence_boost = 0.0;
    cs.is_reversal = false;
    
    if (M.n_cols < 1 || M.n_rows < 5) {
        cs.signal = "neutral";
        return cs;
    }
    
    size_t N = M.n_cols;
    double open = M(1, N-1);
    double high = M(2, N-1);
    double low = M(3, N-1);
    double close = M(4, N-1);
    
    double body = std::fabs(close - open);
    double range = high - low;
    
    if (range < 1e-9) {
        cs.signal = "neutral";
        return cs;
    }
    
    double upper_wick = high - std::max(open, close);
    double lower_wick = std::min(open, close) - low;
    
    bool is_bullish = close > open;
    bool is_bearish = close < open;
    
    // HAMMER: длинная нижняя тень, маленькое тело сверху, бычий разворот
    if (lower_wick > body * 2.0 && upper_wick < body * 0.3 && is_bullish) {
        cs.pattern = CandlePattern::HAMMER;
        cs.pattern_name = "HAMMER";
        cs.signal = "bullish";
        cs.strength = 0.8;
        cs.confidence_boost = 18.0;
        cs.is_reversal = true;
    }
    // INVERTED HAMMER: длинная верхняя тень, маленькое тело снизу
    else if (upper_wick > body * 2.0 && lower_wick < body * 0.3 && is_bullish) {
        cs.pattern = CandlePattern::INVERTED_HAMMER;
        cs.pattern_name = "INVERTED_HAMMER";
        cs.signal = "bullish";
        cs.strength = 0.6;
        cs.confidence_boost = 12.0;
        cs.is_reversal = true;
    }
    // SHOOTING STAR: длинная верхняя тень, медвежий разворот
    else if (upper_wick > body * 2.0 && lower_wick < body * 0.3 && is_bearish) {
        cs.pattern = CandlePattern::SHOOTING_STAR;
        cs.pattern_name = "SHOOTING_STAR";
        cs.signal = "bearish";
        cs.strength = 0.8;
        cs.confidence_boost = 18.0;
        cs.is_reversal = true;
    }
    // HANGING MAN: похож на hammer но медвежий
    else if (lower_wick > body * 2.0 && upper_wick < body * 0.3 && is_bearish) {
        cs.pattern = CandlePattern::HANGING_MAN;
        cs.pattern_name = "HANGING_MAN";
        cs.signal = "bearish";
        cs.strength = 0.6;
        cs.confidence_boost = 12.0;
        cs.is_reversal = true;
    }
    // DOJI: почти нет тела (неопределенность)
    else if (body < range * 0.1) {
        cs.pattern = CandlePattern::DOJI;
        cs.pattern_name = "DOJI";
        cs.signal = "neutral";
        cs.strength = 0.3;
        cs.confidence_boost = -5.0;  // Штраф - нет ясности
        cs.is_reversal = false;
    }
    // MARUBOZU BULL: почти нет теней, сильный бычий тренд
    else if (body > range * 0.9 && is_bullish) {
        cs.pattern = CandlePattern::MARUBOZU_BULL;
        cs.pattern_name = "MARUBOZU_BULL";
        cs.signal = "bullish";
        cs.strength = 0.9;
        cs.confidence_boost = 15.0;
        cs.is_reversal = false;
    }
    // MARUBOZU BEAR: почти нет теней, сильный медвежий тренд
    else if (body > range * 0.9 && is_bearish) {
        cs.pattern = CandlePattern::MARUBOZU_BEAR;
        cs.pattern_name = "MARUBOZU_BEAR";
        cs.signal = "bearish";
        cs.strength = 0.9;
        cs.confidence_boost = 15.0;
        cs.is_reversal = false;
    }
    else {
        cs.signal = "neutral";
    }
    
    return cs;
}

// Определение multi-candle паттернов
inline CandleSignal detect_multi_candle(const arma::mat& M) {
    CandleSignal cs;
    cs.pattern = CandlePattern::NONE;
    cs.confidence_boost = 0.0;
    cs.is_reversal = false;
    
    if (M.n_cols < 3 || M.n_rows < 5) {
        cs.signal = "neutral";
        return cs;
    }
    
    size_t N = M.n_cols;
    
    // Последние 3 свечи
    double o2 = M(1, N-3), h2 = M(2, N-3), l2 = M(3, N-3), c2 = M(4, N-3);
    double o1 = M(1, N-2), h1 = M(2, N-2), l1 = M(3, N-2), c1 = M(4, N-2);
    double o0 = M(1, N-1), h0 = M(2, N-1), l0 = M(3, N-1), c0 = M(4, N-1);
    
    // ENGULFING BULL: текущая зеленая поглощает предыдущую красную
    if (c1 < o1 && c0 > o0 && c0 > o1 && o0 < c1) {
        cs.pattern = CandlePattern::ENGULFING_BULL;
        cs.pattern_name = "ENGULFING_BULL";
        cs.signal = "bullish";
        cs.strength = 0.95;
        cs.confidence_boost = 25.0;  // Очень сильный сигнал!
        cs.is_reversal = true;
        return cs;
    }
    
    // ENGULFING BEAR: текущая красная поглощает предыдущую зеленую
    if (c1 > o1 && c0 < o0 && c0 < o1 && o0 > c1) {
        cs.pattern = CandlePattern::ENGULFING_BEAR;
        cs.pattern_name = "ENGULFING_BEAR";
        cs.signal = "bearish";
        cs.strength = 0.95;
        cs.confidence_boost = 25.0;
        cs.is_reversal = true;
        return cs;
    }
    
    // MORNING STAR: медвежья -> дожи/маленькая -> бычья (разворот вверх)
    double body2 = std::fabs(c2 - o2);
    double body1 = std::fabs(c1 - o1);
    double body0 = std::fabs(c0 - o0);
    
    if (c2 < o2 && body1 < body2 * 0.3 && c0 > o0 && body0 > body2 * 0.5) {
        cs.pattern = CandlePattern::MORNING_STAR;
        cs.pattern_name = "MORNING_STAR";
        cs.signal = "bullish";
        cs.strength = 1.0;
        cs.confidence_boost = 30.0;  // Супер сильный!
        cs.is_reversal = true;
        return cs;
    }
    
    // EVENING STAR: бычья -> дожи/маленькая -> медвежья (разворот вниз)
    if (c2 > o2 && body1 < body2 * 0.3 && c0 < o0 && body0 > body2 * 0.5) {
        cs.pattern = CandlePattern::EVENING_STAR;
        cs.pattern_name = "EVENING_STAR";
        cs.signal = "bearish";
        cs.strength = 1.0;
        cs.confidence_boost = 30.0;
        cs.is_reversal = true;
        return cs;
    }
    
    cs.signal = "neutral";
    return cs;
}

// Главная функция анализа свечей
inline CandleSignal analyze_candles(const arma::mat& M) {
    // Сначала проверяем multi-candle (они сильнее)
    auto multi = detect_multi_candle(M);
    if (multi.pattern != CandlePattern::NONE) {
        return multi;
    }
    
    // Если нет multi-pattern, смотрим single candle
    return detect_single_candle(M);
}

} // namespace etai
