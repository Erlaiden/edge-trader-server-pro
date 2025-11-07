#pragma once
#include <armadillo>
#include "json.hpp"

namespace etai {

// Single-TF inference with logistic policy (already used by /api/infer fallback)
nlohmann::json infer_with_policy(const arma::mat& raw15, const nlohmann::json& model);

// MTF-aware policy inference: uses 15m as core and softly weights by HTFs (60/240/1440)
nlohmann::json infer_with_policy_mtf(const arma::mat& raw15,
                                     const nlohmann::json& model,
                                     const arma::mat* raw60,   int ma60,
                                     const arma::mat* raw240,  int ma240,
                                     const arma::mat* raw1440, int ma1440);

} // namespace etai
