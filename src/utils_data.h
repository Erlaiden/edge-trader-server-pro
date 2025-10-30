#pragma once
#include <armadillo>
#include <string>
#include "json.hpp"

namespace etai {

// Кэш фич X/y. Нужен хотя бы для feat_dim; y может быть заглушкой.
// Возвращает true при успешной загрузке/построении.
bool load_cached_xy(const std::string& symbol,
                    const std::string& interval,
                    arma::mat& X,
                    arma::mat& y);

// Сырые OHLCV из CSV. Режет до первых 6 колонок [ts,open,high,low,close,volume].
// Требование: N >= 300.
bool load_raw_ohlcv(const std::string& symbol,
                    const std::string& interval,
                    arma::mat& out);

// Отчёт по данным для символа/таймфрейма.
nlohmann::json data_health_report(const std::string& symbol,
                                  const std::string& interval);

// Сводка здоровья данных по умолчательному символу на основных ТФ.
nlohmann::json get_data_health();

} // namespace etai
