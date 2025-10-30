#pragma once
#include <armadillo>
#include "json.hpp"

namespace etai {

// Тренер принимает сырой OHLCV (N×6: [ts, open, high, low, close, volume])
// и сам строит фичи/разметку. MTF пока не используем (nullptr).
nlohmann::json trainPPO_pro(const arma::mat& raw15,
                            const arma::mat* raw60,
                            const arma::mat* raw240,
                            const arma::mat* raw1440,
                            int episodes,
                            double tp,
                            double sl,
                            int ma_len);

} // namespace etai
