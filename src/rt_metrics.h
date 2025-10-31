#pragma once
#include <atomic>
#include <string>

namespace etai {

// === Процесс/время ===
extern const long long PROCESS_START_MS;

// === Служебные счётчики ===
extern std::atomic<unsigned long long> REQ_HEALTH;
extern std::atomic<unsigned long long> REQ_BACKFILL;
extern std::atomic<unsigned long long> REQ_TRAIN;
extern std::atomic<unsigned long long> REQ_MODEL;
extern std::atomic<unsigned long long> REQ_INFER;
extern std::atomic<unsigned long long> REQ_METRICS;

// === Тренировки / инфер ===
extern std::atomic<unsigned long long> TRAINS_TOTAL;
extern std::atomic<long long>          LAST_TRAIN_TS;

extern std::atomic<unsigned long long> INFER_SIG_LONG;
extern std::atomic<unsigned long long> INFER_SIG_SHORT;
extern std::atomic<unsigned long long> INFER_SIG_NEUTRAL;

extern std::atomic<long long>          LAST_INFER_TS;     // ms since epoch

// === Данные ===
extern std::atomic<unsigned long long> DATA_ROWS_15;
extern std::atomic<unsigned long long> DATA_ROWS_60;
extern std::atomic<unsigned long long> DATA_ROWS_240;
extern std::atomic<unsigned long long> DATA_ROWS_1440;

// === Модель / трен-агрегаты ===
extern std::atomic<long long>          MODEL_BUILD_TS;
extern std::atomic<unsigned long long> TRAIN_ROWS_USED;

// === CV (на будущее) ===
extern std::atomic<unsigned long long> CV_FOLDS;
extern std::atomic<unsigned long long> CV_EFFECTIVE_FOLDS;
extern std::atomic<double>             CV_IS_SHARPE;
extern std::atomic<double>             CV_OOS_SHARPE;
extern std::atomic<double>             CV_IS_EXPEC;
extern std::atomic<double>             CV_OOS_EXPEC;
extern std::atomic<double>             CV_OOS_DD_MAX;

// === Свежесть ===
extern std::atomic<long long>          DATA_FRESH_MS;

// === Модельные агрегаты для экспорта ===
extern std::atomic<double>             MODEL_BEST_THR;
extern std::atomic<long long>          MODEL_MA_LEN;

// === Последний инференс: скалярные значения ===
extern std::atomic<double>             LAST_INFER_SCORE_AT;   // [-1..1] tanh-политики
extern std::atomic<double>             LAST_INFER_SIGMA_AT;   // волатильность
extern std::atomic<int>                LAST_INFER_SIGNAL_AT;  // -1/0/1

// Утилита
inline std::string bool01(bool v){ return v ? "1" : "0"; }

} // namespace etai
