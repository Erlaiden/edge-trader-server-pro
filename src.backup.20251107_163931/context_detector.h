#pragma once
#include <vector>
#include "json.hpp"

namespace etai {

// Контекст рынка для текущей свечи
struct ContextPoint {
    double energy;        // ATR / SMA(ATR,14)
    double liquidity;     // volume / rolling_max(volume,20)
    double session_sin;   // синус часа (ритм сессий)
    double session_cos;   // косинус часа
    double sentiment;     // прокси-настроения толпы (candle_bias смягчённый)
    int    phase;         // 0=accum, 1=expansion, 2=distribution, 3=correction
};

// Пакет рассчитанных рядов для всего окна
struct ContextSeries {
    std::vector<double> energy;
    std::vector<double> liquidity;
    std::vector<double> session_sin;
    std::vector<double> session_cos;
    std::vector<double> sentiment;
    std::vector<int>    phase;
};

// Основной расчёт контекста (без таймзоны — работаем по Unix-ts в ms)
ContextSeries compute_context(const std::vector<long long>& ts_ms,
                              const std::vector<double>& open,
                              const std::vector<double>& high,
                              const std::vector<double>& low,
                              const std::vector<double>& close,
                              const std::vector<double>& volume);

// Упаковка последних значений в JSON для диагностики
nlohmann::json context_tail_to_json(const std::vector<long long>& ts_ms,
                                    const std::vector<double>& open,
                                    const std::vector<double>& high,
                                    const std::vector<double>& low,
                                    const std::vector<double>& close,
                                    const std::vector<double>& volume);

} // namespace etai
