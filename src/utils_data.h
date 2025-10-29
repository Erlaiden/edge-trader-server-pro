#pragma once
#include <string>
#include "json.hpp"

// Возвращает размер шага таймфрейма в минутах
int minutes_of(const std::string& interval);

// Краткий отчёт по кешированной матрице данных (наличие, строки, разрывы и т.д.)
nlohmann::json data_health_report(const std::string& symbol, const std::string& interval);
