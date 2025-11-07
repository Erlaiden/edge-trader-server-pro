#pragma once
#include <string>
#include <armadillo>
#include "json.hpp"

namespace etai {

// Кэш фичей X/y (как было)
bool load_cached_xy(const std::string& symbol,
                    const std::string& interval,
                    arma::mat& X, arma::mat& y);

// Новый: выбрать путь к raw с приоритетом clean/
std::string select_raw_path(const std::string& symbol,
                            const std::string& interval,
                            bool& used_clean);

// Загрузить OHLCV (N×6) с fallback и усечением до 6 колонок
bool load_raw_ohlcv(const std::string& symbol,
                    const std::string& interval,
                    arma::mat& raw);

// Отчёт по данным (health)
nlohmann::json data_health_report(const std::string& symbol,
                                  const std::string& interval);

nlohmann::json get_data_health();

} // namespace etai
