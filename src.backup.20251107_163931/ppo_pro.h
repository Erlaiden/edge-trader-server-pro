#pragma once
#include <armadillo>
#include "json.hpp"

namespace etai {

// Тренер PPO-PRO (логрег). С поддержкой флага anti-manip (пока может не использоваться).
// raw60/raw240/raw1440 — зарезервированы под MTF, можно передавать nullptr.
nlohmann::json trainPPO_pro(const arma::mat& raw15,
                            const arma::mat* raw60,
                            const arma::mat* raw240,
                            const arma::mat* raw1440,
                            int episodes,
                            double tp,
                            double sl,
                            int ma_len,
                            bool use_antimanip = false);

} // namespace etai
