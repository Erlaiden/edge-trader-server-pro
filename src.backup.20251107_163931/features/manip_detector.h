#pragma once
#include <vector>
namespace etai {
// Флаг ложного пробоя: 1 если был прокол и возврат внутрь диапазона в тот же бар.
std::vector<int> false_break_flags(const std::vector<double>& open,
                                   const std::vector<double>& high,
                                   const std::vector<double>& low,
                                   const std::vector<double>& close,
                                   const std::vector<double>& sup,    // rolling support
                                   const std::vector<double>& res,    // rolling resistance
                                   double tol = 0.0005);              // 5 бп допуск
// trap_index: величина "ловушки" по тени свечи (0..1)
std::vector<double> trap_index_series(const std::vector<double>& open,
                                      const std::vector<double>& high,
                                      const std::vector<double>& low,
                                      const std::vector<double>& close);
}
