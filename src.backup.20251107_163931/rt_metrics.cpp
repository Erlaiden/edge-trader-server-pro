#include "rt_metrics.h"
#include <ctime>

// Процесс
const long long PROCESS_START_MS = (long long)time(nullptr) * 1000;

// Служебные счётчики
std::atomic<unsigned long long> REQ_HEALTH{0};
std::atomic<unsigned long long> REQ_BACKFILL{0};
std::atomic<unsigned long long> REQ_TRAIN{0};
std::atomic<unsigned long long> REQ_MODEL{0};
std::atomic<unsigned long long> REQ_INFER{0};
std::atomic<unsigned long long> REQ_METRICS{0};

std::atomic<unsigned long long> TRAINS_TOTAL{0};
std::atomic<long long>          LAST_TRAIN_TS{0};
std::atomic<unsigned long long> INFER_SIG_LONG{0};
std::atomic<unsigned long long> INFER_SIG_SHORT{0};
std::atomic<unsigned long long> INFER_SIG_NEUTRAL{0};

// Данные/инфер
std::atomic<unsigned long long> DATA_ROWS_15{0};
std::atomic<unsigned long long> DATA_ROWS_60{0};
std::atomic<unsigned long long> DATA_ROWS_240{0};
std::atomic<unsigned long long> DATA_ROWS_1440{0};

std::atomic<long long>          MODEL_BUILD_TS{0};
std::atomic<unsigned long long> TRAIN_ROWS_USED{0};
std::atomic<long long>          LAST_INFER_TS{0};

// CV
std::atomic<unsigned long long> CV_FOLDS{0};
std::atomic<unsigned long long> CV_EFFECTIVE_FOLDS{0};
std::atomic<double>             CV_IS_SHARPE{0.0};
std::atomic<double>             CV_OOS_SHARPE{0.0};
std::atomic<double>             CV_IS_EXPEC{0.0};
std::atomic<double>             CV_OOS_EXPEC{0.0};
std::atomic<double>             CV_OOS_DD_MAX{0.0};

// Свежесть
std::atomic<long long>          DATA_FRESH_MS{0};

// Модельные агрегаты
std::atomic<double>             MODEL_BEST_THR{0.0};
std::atomic<long long>          MODEL_MA_LEN{0};
