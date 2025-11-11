#pragma once
#include <armadillo>
#include <string>
#include <cmath>

namespace etai {

enum class VolatilityRegime {
    ULTRA_LOW,    // ATR < 0.5% - dead market
    LOW,          // ATR 0.5-1.0% - calm
    NORMAL,       // ATR 1.0-2.0% - normal
    HIGH,         // ATR 2.0-4.0% - volatile
    EXTREME       // ATR > 4.0% - crazy market
};

struct VolatilityParams {
    double tp_multiplier;           // Множитель для TP
    double sl_multiplier;           // Множитель для SL
    double confidence_adjustment;   // Корректировка порога confidence
    int max_trades_per_hour;        // Ограничение частоты
    std::string description;
};

// Определение режима волатильности
inline VolatilityRegime detect_volatility_regime(double atr, double current_price) {
    if (current_price <= 0) return VolatilityRegime::NORMAL;
    
    double atr_percent = (atr / current_price) * 100.0;
    
    if (atr_percent < 0.5) return VolatilityRegime::ULTRA_LOW;
    if (atr_percent < 1.0) return VolatilityRegime::LOW;
    if (atr_percent < 2.0) return VolatilityRegime::NORMAL;
    if (atr_percent < 4.0) return VolatilityRegime::HIGH;
    return VolatilityRegime::EXTREME;
}

// Получение параметров для режима
inline VolatilityParams get_volatility_params(VolatilityRegime regime) {
    switch (regime) {
        case VolatilityRegime::ULTRA_LOW:
            // Скучный рынок - не торгуем много
            return {
                0.8,    // TP меньше (маленькие движения)
                0.8,    // SL меньше
                +10.0,  // Порог ВЫШЕ (нужна большая уверенность)
                1,      // Максимум 1 сделка в час
                "Dead market - minimal trading"
            };
            
        case VolatilityRegime::LOW:
            // Спокойный рынок - стандарт
            return {
                1.0,    // Стандартные TP/SL
                1.0,
                +5.0,   // Порог немного выше
                2,      // 2 сделки в час
                "Calm market - standard params"
            };
            
        case VolatilityRegime::NORMAL:
            // Нормальная волатильность - АГРЕССИВНО!
            return {
                1.2,    // TP больше
                0.9,    // SL немного меньше (риск-менеджмент)
                -5.0,   // Порог НИЖЕ (более агрессивно)
                4,      // До 4 сделок в час
                "Normal volatility - aggressive trading"
            };
            
        case VolatilityRegime::HIGH:
            // Высокая волатильность - широкие стопы
            return {
                1.5,    // TP намного больше
                1.3,    // SL шире (не вылететь на шуме)
                0.0,    // Стандартный порог
                3,      // 3 сделки в час
                "High volatility - wide stops"
            };
            
        case VolatilityRegime::EXTREME:
            // Бешеный рынок - ОСТОРОЖНО!
            return {
                2.0,    // TP очень большой
                1.5,    // SL очень широкий
                +15.0,  // Порог НАМНОГО выше
                2,      // Только 2 сделки в час
                "EXTREME volatility - careful!"
            };
    }
    
    return {1.0, 1.0, 0.0, 3, "Unknown"};
}

// Конвертация в строку
inline std::string volatility_regime_to_string(VolatilityRegime regime) {
    switch (regime) {
        case VolatilityRegime::ULTRA_LOW: return "ULTRA_LOW";
        case VolatilityRegime::LOW: return "LOW";
        case VolatilityRegime::NORMAL: return "NORMAL";
        case VolatilityRegime::HIGH: return "HIGH";
        case VolatilityRegime::EXTREME: return "EXTREME";
        default: return "UNKNOWN";
    }
}

// Time-of-Day анализ
enum class TradingSession {
    ASIAN,      // 00:00-08:00 UTC - низкая ликвидность
    EUROPEAN,   // 08:00-16:00 UTC (кроме overlap)
    US,         // 16:00-24:00 UTC (кроме overlap)
    OVERLAP     // 12:00-16:00 UTC - максимальная ликвидность
};

inline TradingSession get_trading_session() {
    time_t now = time(nullptr);
    struct tm* utc = gmtime(&now);
    int hour = utc->tm_hour;
    
    if (hour >= 0 && hour < 8) return TradingSession::ASIAN;
    if (hour >= 12 && hour < 16) return TradingSession::OVERLAP;
    if (hour >= 8 && hour < 16) return TradingSession::EUROPEAN;
    return TradingSession::US;
}

inline std::string session_to_string(TradingSession session) {
    switch (session) {
        case TradingSession::ASIAN: return "ASIAN";
        case TradingSession::EUROPEAN: return "EUROPEAN";
        case TradingSession::US: return "US";
        case TradingSession::OVERLAP: return "OVERLAP";
        default: return "UNKNOWN";
    }
}

// Корректировка порога по времени суток
inline double get_session_threshold_adjustment(TradingSession session) {
    switch (session) {
        case TradingSession::ASIAN:
            // Азиатская сессия - низкая ликвидность
            return +8.0;  // Повышаем порог
            
        case TradingSession::EUROPEAN:
            // Европейская - средняя активность
            return 0.0;  // Стандарт
            
        case TradingSession::US:
            // Американская - высокая активность
            return -3.0;  // Снижаем порог
            
        case TradingSession::OVERLAP:
            // Оверлэп - ЛУЧШЕЕ время для торговли
            return -5.0;  // Сильно снижаем порог
    }
    return 0.0;
}

// Комплексный анализ: Volatility + Time of Day
struct MarketConditions {
    VolatilityRegime vol_regime;
    TradingSession session;
    double vol_adjustment;      // Корректировка confidence threshold
    double session_adjustment;  // Корректировка от времени суток
    double total_adjustment;    // Итоговая корректировка
    double tp_multiplier;
    double sl_multiplier;
    int max_trades_per_hour;
    std::string description;
};

inline MarketConditions analyze_market_conditions(double atr, double current_price) {
    MarketConditions mc;
    
    mc.vol_regime = detect_volatility_regime(atr, current_price);
    mc.session = get_trading_session();
    
    auto vol_params = get_volatility_params(mc.vol_regime);
    
    mc.vol_adjustment = vol_params.confidence_adjustment;
    mc.session_adjustment = get_session_threshold_adjustment(mc.session);
    mc.total_adjustment = mc.vol_adjustment + mc.session_adjustment;
    
    mc.tp_multiplier = vol_params.tp_multiplier;
    mc.sl_multiplier = vol_params.sl_multiplier;
    mc.max_trades_per_hour = vol_params.max_trades_per_hour;
    
    mc.description = vol_params.description + " | Session: " + session_to_string(mc.session);
    
    std::cout << "[MARKET_CONDITIONS] Volatility: " << volatility_regime_to_string(mc.vol_regime)
              << " | Session: " << session_to_string(mc.session)
              << " | Threshold adj: " << mc.total_adjustment << "%" << std::endl;
    
    return mc;
}

} // namespace etai
