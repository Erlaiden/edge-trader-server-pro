#pragma once
#include <armadillo>

namespace etai {

// Sharpe по ряду доходностей r; annualizer=1.0 для per-trade шкалы.
double calc_sharpe(const arma::vec& r, double eps = 1e-12, double annualizer = 1.0);

// Максимальная просадка по кумулятивной эквити из r.
double calc_max_drawdown(const arma::vec& r);

// Winrate: доля r > 0.
double calc_winrate(const arma::vec& r);

} // namespace etai
