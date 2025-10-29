#pragma once
#include "json.hpp"
#include <armadillo>

namespace etai {

// ВНУТРЕННЯЯ оценочная функция для PRO-поиска параметров.
// Не пишет модель, не является публичным режимом.
nlohmann::json evalPPO_internal(const arma::mat& M,
                                int episodes,
                                double tp_pct,
                                double sl_pct,
                                int ma_len);

// Инференс на одном ТФ (15m), с волатильностным фильтром
nlohmann::json infer_with_threshold(const arma::mat& M,
                                    double best_thr,
                                    int ma_len);

// Мульти-таймфреймный инференс (15m + фильтры 60m/240m/1440m)
nlohmann::json infer_mtf(const arma::mat& M15, double thr15, int ma15,
                         const arma::mat* M60,   int ma60,
                         const arma::mat* M240,  int ma240,
                         const arma::mat* M1440, int ma1440);

// Batch-вариант для карты сигналов (последние N баров 15m с HTF-фильтрами)
nlohmann::json infer_mtf_batch(const arma::mat& M15, double thr15, int ma15,
                               const arma::mat* M60,   int ma60,
                               const arma::mat* M240,  int ma240,
                               const arma::mat* M1440, int ma1440,
                               int last_n);

} // namespace etai
