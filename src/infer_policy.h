#pragma once
#include <armadillo>
#include "json.hpp"

namespace etai {

// Инференс по policy из JSON-модели (если блок "policy" присутствует).
// Возвращает: { ok, signal, score, sigma, vol_threshold }
nlohmann::json infer_with_policy(const arma::mat& M15, const nlohmann::json& model);

} // namespace etai
