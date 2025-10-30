#pragma once
#include "json.hpp"
#include <armadillo>
#include <string>

namespace etai {

// Отчёт по здоровью данных
nlohmann::json data_health_report(const std::string& symbol, const std::string& interval);

// Загрузка кэша признаков/таргета; если кэша нет — строит из base-CSV и сохраняет
bool load_cached_xy(const std::string& symbol,
                    const std::string& interval,
                    arma::mat& X,
                    arma::mat& y);

// СЫРОЙ OHLCV (ts,open,high,low,close,volume) из cache/<SYMBOL>_<INTERVAL>.csv
// Возвращает true если успешно считали в raw (N×6)
bool load_raw_ohlcv(const std::string& symbol,
                    const std::string& interval,
                    arma::mat& raw);

// Агрегированный отчёт по файлам cache/ для health_ai
nlohmann::json get_data_health();

} // namespace etai
