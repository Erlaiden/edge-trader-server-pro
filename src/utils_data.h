#pragma once
#include "json.hpp"
#include <armadillo>
#include <string>

namespace etai {

// Отчёт по здоровью данных (у тебя уже был)
nlohmann::json data_health_report(const std::string& symbol, const std::string& interval);

// === НОВОЕ ОБЪЯВЛЕНИЕ ===
// Загрузка кэша признаков/таргета для обучения
// Возвращает true при успешной загрузке
bool load_cached_xy(const std::string& symbol,
                    const std::string& interval,
                    arma::mat& X,
                    arma::mat& y);

} // namespace etai
