#include "metrics.h"
#include <cmath>

namespace etai {

double calc_sharpe(const arma::vec& r, double eps, double annualizer) {
    if (r.n_elem == 0) return 0.0;
    double mu = arma::mean(r);
    double sd = arma::stddev(r, 0);
    if (!std::isfinite(mu) || !std::isfinite(sd) || sd < eps) return 0.0;
    double s = mu / sd;
    if (std::isfinite(annualizer) && annualizer > 0.0) s *= annualizer;
    return s;
}

double calc_max_drawdown(const arma::vec& r) {
    if (r.n_elem == 0) return 0.0;
    arma::vec eq = arma::cumsum(r);
    double peak = -1e300, dd_max = 0.0;
    for (arma::uword i=0;i<eq.n_elem;++i) {
        double v = eq(i);
        if (v > peak) peak = v;
        double dd = peak - v;
        if (dd > dd_max) dd_max = dd;
    }
    return dd_max;
}

double calc_winrate(const arma::vec& r) {
    if (r.n_elem == 0) return 0.0;
    arma::uword pos=0;
    for (arma::uword i=0;i<r.n_elem;++i) if (r(i)>0.0) ++pos;
    return double(pos)/double(r.n_elem);
}

} // namespace etai
