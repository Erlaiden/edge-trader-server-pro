#pragma once
#include <armadillo>
#include <string>
#include <cmath>

namespace etai {

struct VolumeSignal {
    double current_volume;
    double avg_volume_20;
    double volume_ratio;        // current / avg
    bool is_spike;              // ratio > 2.0
    double obv;                 // On Balance Volume
    double volume_trend;        // MA(5) vs MA(20)
    std::string signal;         // "strong_buy", "strong_sell", "accumulation", "distribution", "neutral"
    double confidence_boost;    // Сколько добавить к confidence
};

// Анализ объема за последние N свечей
inline VolumeSignal analyze_volume(const arma::mat& M, size_t lookback = 20) {
    VolumeSignal vs;
    
    if (M.n_cols < lookback + 1 || M.n_rows < 6) {
        vs.signal = "neutral";
        vs.confidence_boost = 0.0;
        return vs;
    }
    
    size_t N = M.n_cols;
    
    // 1. Текущий объем и средний
    vs.current_volume = M(5, N-1);
    
    double sum = 0.0;
    for (size_t i = N-lookback; i < N; ++i) {
        sum += M(5, i);
    }
    vs.avg_volume_20 = sum / lookback;
    
    vs.volume_ratio = vs.current_volume / (vs.avg_volume_20 > 0 ? vs.avg_volume_20 : 1.0);
    vs.is_spike = vs.volume_ratio > 2.0;
    
    // 2. On Balance Volume (OBV)
    vs.obv = 0.0;
    for (size_t i = 1; i < N; ++i) {
        double close = M(4, i);
        double prev_close = M(4, i-1);
        double vol = M(5, i);
        
        if (close > prev_close) {
            vs.obv += vol;
        } else if (close < prev_close) {
            vs.obv -= vol;
        }
    }
    
    // 3. Volume Trend (короткая MA vs длинная MA)
    if (N > 5) {
        double vol_ma5 = 0.0;
        for (size_t i = N-5; i < N; ++i) {
            vol_ma5 += M(5, i);
        }
        vol_ma5 /= 5.0;
        
        vs.volume_trend = (vol_ma5 - vs.avg_volume_20) / (vs.avg_volume_20 > 0 ? vs.avg_volume_20 : 1.0);
    } else {
        vs.volume_trend = 0.0;
    }
    
    // 4. Определение сигнала и confidence boost
    vs.confidence_boost = 0.0;
    
    // Проверяем направление цены
    double price_change = M(4, N-1) - M(4, N-2);
    bool price_up = price_change > 0;
    bool price_down = price_change < 0;
    
    // STRONG BUY: высокий объем + растущая цена + положительный OBV
    if (vs.volume_ratio > 1.5 && price_up && vs.obv > 0) {
        vs.signal = "strong_buy";
        vs.confidence_boost = 15.0;
        
        if (vs.is_spike) {
            vs.confidence_boost += 10.0;  // Супер сильный сигнал!
        }
    }
    // STRONG SELL: высокий объем + падающая цена + отрицательный OBV
    else if (vs.volume_ratio > 1.5 && price_down && vs.obv < 0) {
        vs.signal = "strong_sell";
        vs.confidence_boost = 15.0;
        
        if (vs.is_spike) {
            vs.confidence_boost += 10.0;
        }
    }
    // ACCUMULATION: OBV растет + объем выше среднего
    else if (vs.obv > 0 && vs.volume_ratio > 1.0 && vs.volume_trend > 0) {
        vs.signal = "accumulation";
        vs.confidence_boost = 8.0;
    }
    // DISTRIBUTION: OBV падает + объем выше среднего
    else if (vs.obv < 0 && vs.volume_ratio > 1.0 && vs.volume_trend > 0) {
        vs.signal = "distribution";
        vs.confidence_boost = 8.0;
    }
    // LOW VOLUME WARNING: объем слишком низкий - подозрительно
    else if (vs.volume_ratio < 0.5) {
        vs.signal = "low_volume_warning";
        vs.confidence_boost = -10.0;  // ШТРАФ за низкий объем!
    }
    // NEUTRAL
    else {
        vs.signal = "neutral";
        vs.confidence_boost = 0.0;
    }
    
    return vs;
}

// Детекция дивергенции: цена идет вверх, а OBV вниз (bearish divergence)
inline bool detect_volume_divergence(const arma::mat& M, size_t lookback = 10) {
    if (M.n_cols < lookback + 1 || M.n_rows < 6) return false;
    
    size_t N = M.n_cols;
    
    // Считаем OBV для последних N свечей
    std::vector<double> obv_values;
    double obv = 0.0;
    
    for (size_t i = N-lookback; i < N; ++i) {
        if (i > 0) {
            double close = M(4, i);
            double prev_close = M(4, i-1);
            double vol = M(5, i);
            
            if (close > prev_close) obv += vol;
            else if (close < prev_close) obv -= vol;
        }
        obv_values.push_back(obv);
    }
    
    // Проверяем: цена растет, OBV падает?
    double price_start = M(4, N-lookback);
    double price_end = M(4, N-1);
    double obv_start = obv_values.front();
    double obv_end = obv_values.back();
    
    // Bearish divergence
    if (price_end > price_start && obv_end < obv_start) {
        return true;  // Медвежья дивергенция - цена скоро упадет
    }
    
    // Bullish divergence
    if (price_end < price_start && obv_end > obv_start) {
        return true;  // Бычья дивергенция - цена скоро вырастет
    }
    
    return false;
}

} // namespace etai
