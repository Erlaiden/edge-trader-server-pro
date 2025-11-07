#pragma once
#include <atomic>
#include <string>

// ТОЛЬКО extern-декларации. Определения — в rt_metrics.cpp
extern const long long PROCESS_START_MS;

// Служебные счётчики
extern std::atomic<unsigned long long> REQ_HEALTH;
extern std::atomic<unsigned long long> REQ_BACKFILL;
extern std::atomic<unsigned long long> REQ_TRAIN;
extern std::atomic<unsigned long long> REQ_MODEL;
extern std::atomic<unsigned long long> REQ_INFER;
extern std::atomic<unsigned long long> REQ_METRICS;

extern std::atomic<unsigned long long> TRAINS_TOTAL;
extern std::atomic<long long>          LAST_TRAIN_TS;
extern std::atomic<unsigned long long> INFER_SIG_LONG;
extern std::atomic<unsigned long long> INFER_SIG_SHORT;
extern std::atomic<unsigned long long> INFER_SIG_NEUTRAL;

// Данные/инфер
extern std::atomic<unsigned long long> DATA_ROWS_15;
extern std::atomic<unsigned long long> DATA_ROWS_60;
extern std::atomic<unsigned long long> DATA_ROWS_240;
extern std::atomic<unsigned long long> DATA_ROWS_1440;

extern std::atomic<long long>          MODEL_BUILD_TS;
extern std::atomic<unsigned long long> TRAIN_ROWS_USED;
extern std::atomic<long long>          LAST_INFER_TS;

// CV
extern std::atomic<unsigned long long> CV_FOLDS;
extern std::atomic<unsigned long long> CV_EFFECTIVE_FOLDS;
extern std::atomic<double>             CV_IS_SHARPE;
extern std::atomic<double>             CV_OOS_SHARPE;
extern std::atomic<double>             CV_IS_EXPEC;
extern std::atomic<double>             CV_OOS_EXPEC;
extern std::atomic<double>             CV_OOS_DD_MAX;

// Свежесть
extern std::atomic<long long>          DATA_FRESH_MS;

// Модельные агрегаты
extern std::atomic<double>             MODEL_BEST_THR;
extern std::atomic<long long>          MODEL_MA_LEN;

// Утилита (оставим, вдруг нужна)
inline std::string bool01(bool v){ return v ? "1" : "0"; }
