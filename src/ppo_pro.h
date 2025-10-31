#pragma once
#include <armadillo>
#include "json.hpp"

namespace etai {

// PPO-PRO (логрег-политика), v7 (режим+импульс).
// Параметр use_antimanip оставлен для совместимости с вызовом, в v7 можно не использовать.
nlohmann::json trainPPO_pro(const arma::mat& raw15,
                            const arma::mat* raw60,
                            const arma::mat* raw240,
                            const arma::mat* raw1440,
                            int episodes,
                            double tp,
                            double sl,
                            int ma_len,
                            bool use_antimanip);

} // namespace etai
