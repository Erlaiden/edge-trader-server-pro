#pragma once
#include "json.hpp"

// Реализации accessor-функций, которых не хватало линковщику.
// Нейтральные, без побочных эффектов. Все в namespace etai.

namespace etai {

/// Текущая модель (читается с диска из cache/models/<SYMBOL>_<INTERVAL>_ppo_pro.json)
/// SYMBOL берется из ETAI_SYMBOL или "BTCUSDT"; INTERVAL из ETAI_INTERVAL или "15".
/// Возвращает {} если файла нет или ошибка чтения.
nlohmann::json get_current_model();

/// Текущий лучший порог модели (best_thr). Берется из атомика MODEL_BEST_THR.
/// Если атомик не инициализирован, вернёт 0.0.
double get_model_thr();

/// Текущая длина MA модели (ma_len). Берется из атомика MODEL_MA_LEN.
/// Если не инициализирован, вернёт 0.
long long get_model_ma_len();

/// Сводка по данным (health). Делает отчёт по 15/60/240/1440 для SYMBOL.
/// SYMBOL берется из ETAI_SYMBOL или "BTCUSDT".
nlohmann::json get_data_health();

} // namespace etai
