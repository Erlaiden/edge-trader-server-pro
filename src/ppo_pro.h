#pragma once
#include <armadillo>
#include "json.hpp"  // проектный nlohmann::json

namespace etai {

// Тренировка PPO_pro v1 на сыром OHLCV. Возвращает JSON с моделью и метриками.
nlohmann::json trainPPO_pro(
    const arma::mat& raw15,
    const arma::mat* raw60,
    const arma::mat* raw240,
    const arma::mat* raw1440,
    int episodes,
    double tp,
    double sl,
    int ma_len);

} // namespace etai
