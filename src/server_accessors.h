#pragma once
#include "json.hpp"

// Набор accessor-функций для health_ai и инициализации модельных атомиков.
namespace etai {

// Текущая модель: чтение с диска из cache/models/<SYMBOL>_<INTERVAL>_ppo_pro.json
// SYMBOL из ETAI_SYMBOL или "BTCUSDT"; INTERVAL из ETAI_INTERVAL или "15".
// Возвращает {} если файла нет или ошибка чтения.
nlohmann::json get_current_model();

// Текущий лучший порог модели и MA-длина из атомиков (0 если не инициализировано).
double     get_model_thr();
long long  get_model_ma_len();

// Сводка по данным по всем МТF для SYMBOL.
nlohmann::json get_data_health();

// Инициализация атомиков модели с диска. Безопасно вызывать на старте.
// Если модель существует, выставит MODEL_BEST_THR, MODEL_MA_LEN, MODEL_BUILD_TS.
void init_model_atoms_from_disk();

} // namespace etai
