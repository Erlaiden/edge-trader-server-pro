#include "support_resistance.h"
#include <algorithm>
#include <cmath>
namespace etai {

static inline double vmin(const std::vector<double>& v, int a, int b){
    double m = INFINITY;
    for(int i=a;i<=b;++i) m = std::min(m, v[i]);
    return std::isfinite(m)?m:0.0;
}
static inline double vmax(const std::vector<double>& v, int a, int b){
    double m = -INFINITY;
    for(int i=a;i<=b;++i) m = std::max(m, v[i]);
    return std::isfinite(m)?m:0.0;
}

std::vector<double> rolling_support(const std::vector<double>& low, int win){
    const int n = (int)low.size();
    std::vector<double> sup(n, 0.0);
    if(n==0 || win<=1) return sup;
    for(int i=0;i<n;++i){
        int a = std::max(0, i - win + 1);
        sup[i] = vmin(low, a, i);
    }
    return sup;
}
std::vector<double> rolling_resistance(const std::vector<double>& high, int win){
    const int n = (int)high.size();
    std::vector<double> res(n, 0.0);
    if(n==0 || win<=1) return res;
    for(int i=0;i<n;++i){
        int a = std::max(0, i - win + 1);
        res[i] = vmax(high, a, i);
    }
    return res;
}

} // namespace etai
