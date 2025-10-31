#include "manip_detector.h"
#include <cmath>
#include <algorithm>
namespace etai {

std::vector<int> false_break_flags(const std::vector<double>& open,
                                   const std::vector<double>& high,
                                   const std::vector<double>& low,
                                   const std::vector<double>& close,
                                   const std::vector<double>& sup,
                                   const std::vector<double>& res,
                                   double tol)
{
    const size_t n = close.size();
    std::vector<int> f(n, 0);
    if(n==0) return f;
    for(size_t i=1;i<n;++i){
        double s = sup[i-1];
        double r = res[i-1];
        // ложный пробой вверх: high вышел выше r+tol, но закрылись обратно ниже r
        bool fake_up = (r>0 && high[i] > r*(1.0+tol) && close[i] < r);
        // ложный пробой вниз: low ушёл ниже s-tol, но закрылись выше s
        bool fake_dn = (s>0 && low[i]  < s*(1.0-tol) && close[i] > s);
        f[i] = (fake_up || fake_dn) ? 1 : 0;
    }
    return f;
}

std::vector<double> trap_index_series(const std::vector<double>& open,
                                      const std::vector<double>& high,
                                      const std::vector<double>& low,
                                      const std::vector<double>& close)
{
    const size_t n = close.size();
    std::vector<double> t(n, 0.0);
    for(size_t i=0;i<n;++i){
        double range = std::max(1e-12, high[i]-low[i]);
        double body  = std::fabs(close[i]-open[i]);
        double upper_wick = std::max(0.0, high[i]-std::max(close[i],open[i]));
        double lower_wick = std::max(0.0, std::min(close[i],open[i])-low[i]);
        // ловушка — большая тень при маленьком теле
        double wick_ratio = (upper_wick+lower_wick)/range;
        double body_ratio = body/range;
        double idx = std::max(0.0, wick_ratio - body_ratio); // 0..1 примерно
        t[i] = std::min(1.0, idx);
    }
    return t;
}

} // namespace etai
