#pragma once
#include "json.hpp"
#include <string>

namespace etai {

// Запускает обучение PPO_PRO и сохраняет модель на диск.
// Возвращает JSON: { ok, model_path, metrics:{best_thr,val_accuracy,val_reward}, error, error_detail }
nlohmann::json run_train_pro_and_save(const std::string& symbol,
                                      const std::string& interval,
                                      int episodes,
                                      double tp,
                                      double sl,
                                      int ma_len);

} // namespace etai
