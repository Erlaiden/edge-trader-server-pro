#pragma once
#include "json.hpp"
#include <armadillo>

namespace etai {
// Расширенная PPO (PRO): совместимая сигнатура, M60/M240/M1440 могут быть nullptr
nlohmann::json trainPPO_pro(const arma::mat& M15,
                            const arma::mat* M60,
                            const arma::mat* M240,
                            const arma::mat* M1440,
                            int episodes,
                            double tp_pct,
                            double sl_pct,
                            int ma_len);
} // namespace etai
