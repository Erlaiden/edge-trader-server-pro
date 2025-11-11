#pragma once
#include "../json.hpp"
#include <armadillo>
#include <string>

namespace etai {

enum class MarketRegime {
    STRONG_UPTREND,      // Daily+4H+1H UP - только LONG
    STRONG_DOWNTREND,    // Daily+4H+1H DOWN - только SHORT
    RANGE_BOUND,         // Флет - mean reversion
    CORRECTION_UP,       // Коррекция в апе - LONG на развороте
    CORRECTION_DOWN,     // Коррекция в дауне - SHORT на развороте
    BREAKOUT_UP,         // Пробой вверх - LONG сразу
    BREAKOUT_DOWN,       // Пробой вниз - SHORT сразу
    MANIPULATION,        // Манипуляция - не торгуем
    UNCERTAIN            // Непонятно - не торгуем
};

struct RegimeParams {
    bool allow_long;
    bool allow_short;
    double tp_percent;
    double sl_percent;
    double min_confidence;
    std::string note;
    bool use_mean_reversion;  // Для флета
};

// Главная функция определения режима
MarketRegime detect_regime(
    const nlohmann::json& htf,
    const arma::mat& M15,
    double atr,
    double avg_atr
);

// Получить параметры для режима
RegimeParams get_regime_params(MarketRegime regime);

// Проверка на манипуляцию
bool is_manipulation(const arma::mat& M15);

// Проверка на пробой
bool is_breakout(const arma::mat& M15, double atr, double avg_atr);

// Проверка на флет
bool is_ranging(const arma::mat& M15, double atr, double avg_atr);

// Определение позиции цены в канале (для флета)
double get_channel_position(const arma::mat& M15, double atr);

// Конвертация enum в строку
std::string regime_to_string(MarketRegime regime);

} // namespace etai
