#pragma once
#include <armadillo>

namespace etai {

// Sharpe по ряду доходностей r (на шаг): mean/std.
// annualizer — множитель для скейлинга (опционально). Для per-trade ставим 1.0.
double calc_sharpe(const arma::vec& r, double eps = 1e-12, double annualizer = 1.0);

// Максимальная просадка по кривой эквити (кумулятивная сумма доходностей).
double calc_max_drawdown(const arma::vec& r);

// Winrate: доля r > 0.
double calc_winrate(const arma::vec& r);

}
