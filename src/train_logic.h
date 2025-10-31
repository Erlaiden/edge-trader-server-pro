#pragma once
#include "json.hpp"
#include <string>

namespace etai {

// Запуск тренировки с сохранением модели на диск
nlohmann::json run_train_pro_and_save(const std::string& symbol,
                                      const std::string& interval,
                                      int episodes,
                                      double tp,
                                      double sl,
                                      int ma_len,
                                      bool use_antimanip);

} // namespace etai
