#pragma once
#include <string>
#include "json.hpp"

// Точка входа тренировки PRO с сохранением модели и обновлением атомиков/метрик.
// Полностью соответствует прежней логике.
nlohmann::json run_train_pro_and_save(const std::string& symbol,
                                      const std::string& interval,
                                      int episodes, double tp, double sl, int ma_len);
