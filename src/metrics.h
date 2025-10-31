#pragma once
#include <armadillo>

namespace etai {

double calc_sharpe(const arma::vec& r, double eps = 1e-12, double annualizer = 1.0);
double calc_max_drawdown(const arma::vec& r);
double calc_winrate(const arma::vec& r);

} // namespace etai
