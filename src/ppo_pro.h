#pragma once
#include <armadillo>
#include "json.hpp"

namespace etai {

// Тренер PPO-PRO (логрег-политика) с поддержкой анти-манип фильтра.
// use_antimanip=true — исключать примеры, где manip_flag==1 (из feature matrix v6).
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
