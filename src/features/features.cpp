#include "features.h"
#include "json.hpp"
#include <armadillo>
#include <cmath>
#include <algorithm>
#include <vector>
#include <numeric>

using json = nlohmann::json;

namespace etai {

constexpr int FEAT_VERSION = 6;

// ---------- helpers ----------
static double ema_one(const std::vector<double>& v, int period, size_t i) {
    if (i + 1 < (size_t)period) return NAN;
    double k = 2.0 / (period + 1);
    double e = v[i - period + 1];
    for (size_t j = i - period + 2; j <= i; ++j) e = v[j] * k + e * (1.0 - k);
    return e;
}
static double sma_one(const std::vector<double>& v, int period, size_t i) {
    if (i + 1 < (size_t)period) return NAN;
    double s = 0; for (size_t j = i + 1 - period; j <= i; ++j) s += v[j];
    return s / period;
}
static double rsi_one(const std::vector<double>& c, int period, size_t i) {
    if (i + 1 < (size_t)period + 1) return NAN;
    double gain = 0, loss = 0;
    for (size_t j = i + 1 - period; j <= i; ++j) {
        double d = c[j] - c[j - 1];
        if (d >= 0) gain += d; else loss -= d;
    }
    if (loss == 0) return 100.0;
    double rs = gain / loss;
    return 100.0 - (100.0 / (1.0 + rs));
}
static double atr_one(const std::vector<double>& h,
                      const std::vector<double>& l,
                      const std::vector<double>& c,
                      int period, size_t i) {
    if (i + 1 < (size_t)period + 1) return NAN;
    double s = 0;
    for (size_t j = i + 1 - period; j <= i; ++j) {
        double tr = std::max({h[j] - l[j], std::fabs(h[j] - c[j - 1]), std::fabs(l[j] - c[j - 1])});
        s += tr;
    }
    return s / period;
}
static inline double clampd(double v, double lo, double hi){
    if(!std::isfinite(v)) return 0.0;
    if(v < lo) return lo;
    if(v > hi) return hi;
    return v;
}

// ---------- main builder ----------
arma::Mat<double> build_feature_matrix(const arma::Mat<double>& raw) {
    if (raw.n_rows < 30 || raw.n_cols < 6) return arma::Mat<double>();
    const size_t n = raw.n_rows;

    std::vector<double> open(n), high(n), low(n), close(n), vol(n);
    for (size_t i=0;i<n;++i){
        open[i]=raw(i,1); high[i]=raw(i,2); low[i]=raw(i,3); close[i]=raw(i,4); vol[i]=raw(i,5);
    }

    std::vector<double> ema_fast(n), ema_slow(n), rsi_v(n), macd_v(n),
        macd_signal(n), macd_hist(n), atr_v(n), bb_width(n), volr_v(n),
        corr_v(n), accel_v(n), slope_v(n),
        dist_sup(n), dist_res(n), flat_flag(n), trend_up(n), trend_dn(n),
        vol_anom(n), wick_ratio(n), vol_price_div(n), gap_jump(n), manip_flag(n);

    // базовые фичи (v5)
    for (size_t i=0;i<n;++i){
        ema_fast[i]=ema_one(close,12,i);
        ema_slow[i]=ema_one(close,26,i);
        rsi_v[i]=rsi_one(close,14,i);
        atr_v[i]=atr_one(high,low,close,14,i);
        if (i>=20){
            double sma20=sma_one(close,20,i), sd=0;
            for(size_t j=i+1-20;j<=i;++j) sd+=std::pow(close[j]-sma20,2);
            sd=std::sqrt(sd/20);
            bb_width[i]=(sma20!=0)?(4.0*sd/sma20):NAN;
        }else bb_width[i]=NAN;
        volr_v[i]=(i>=20 && sma_one(vol,20,i)>0)?vol[i]/sma_one(vol,20,i):NAN;
    }

    for (size_t i=0;i<n;++i){
        macd_v[i]=ema_fast[i]-ema_slow[i];
        macd_signal[i]=ema_one(macd_v,9,i);
        macd_hist[i]=macd_v[i]-macd_signal[i];
    }

    for (size_t i=1;i<n;++i){
        double atr_now=(std::isfinite(atr_v[i]) && atr_v[i]>0)?atr_v[i]:1e-8;
        corr_v[i]=clampd((close[i]-ema_slow[i])/atr_now,-3,3);
        double mom1=(close[i]-close[i-1])/close[i-1];
        double mom2=(i>1)?(close[i-1]-close[i-2])/close[i-2]:0.0;
        accel_v[i]=clampd(mom1-mom2,-0.05,0.05);
        slope_v[i]=clampd((ema_fast[i]-ema_fast[i-1])/std::max(1e-8,ema_fast[i]),-0.05,0.05);
    }

    // regime + SR
    const int SR_WIN=48; const double FLAT_K=0.3;
    for(size_t i=0;i<n;++i){
        size_t l=(i+1>SR_WIN)?(i+1-SR_WIN):0, r=i;
        double hh=-1e300,ll=1e300;
        for(size_t j=l;j<=r;++j){hh=std::max(hh,high[j]); ll=std::min(ll,low[j]);}
        double atr_now=(std::isfinite(atr_v[i])&&atr_v[i]>0)?atr_v[i]:1e-8;
        dist_res[i]=clampd((hh-close[i])/atr_now,0,5);
        dist_sup[i]=clampd((close[i]-ll)/atr_now,0,5);
        double spread=ema_fast[i]-ema_slow[i];
        double flat_thr=FLAT_K*atr_now;
        bool is_flat=std::fabs(spread)<flat_thr && std::fabs(slope_v[i])<0.002;
        flat_flag[i]=is_flat?1.0:0.0;
        trend_up[i]=(!is_flat && spread>0 && slope_v[i]>0)?1.0:0.0;
        trend_dn[i]=(!is_flat && spread<0 && slope_v[i]<0)?1.0:0.0;
    }

    // ----------- Anti-manip -----------
    for(size_t i=2;i<n;++i){
        double atr_now=(std::isfinite(atr_v[i])&&atr_v[i]>0)?atr_v[i]:1e-8;

        // 1) vol_anomaly
        double meanv=sma_one(vol,20,i);
        double sdv=0; for(size_t j=i+1-20;j<=i;++j) sdv+=std::pow(vol[j]-meanv,2);
        sdv=std::sqrt(sdv/20);
        vol_anom[i]=clampd((vol[i]-meanv)/(sdv+1e-9),-5,5);

        // 2) wick_ratio
        double body=std::fabs(close[i]-open[i]);
        double wick=high[i]-low[i];
        wick_ratio[i]=clampd((wick>0)?(wick/body):0,0,10);

        // 3) vol_price_div
        double dir=(close[i]>open[i])?1.0:-1.0;
        vol_price_div[i]=(vol_anom[i]>1.5 && dir<0)?1.0:((vol_anom[i]<-1.5 && dir>0)?-1.0:0.0);

        // 4) gap_jump
        double gap=(open[i]-close[i-1])/std::max(1e-8,close[i-1]);
        gap_jump[i]=clampd(gap/atr_now,-3,3);

        // общий флаг
        int count=0;
        if(std::fabs(vol_anom[i])>2.0) count++;
        if(wick_ratio[i]>3.0) count++;
        if(std::fabs(vol_price_div[i])>0.5) count++;
        if(std::fabs(gap_jump[i])>1.0) count++;
        manip_flag[i]=(count>=2)?1.0:0.0;
    }

    // ----------- Matrix assembly -----------
    const size_t D=20;
    arma::Mat<double> F(n,D,arma::fill::zeros);
    for(size_t i=0;i<n;++i){
        F(i,0)=ema_fast[i]-ema_slow[i];
        F(i,1)=rsi_v[i]/100.0;
        F(i,2)=macd_v[i];
        F(i,3)=macd_hist[i];
        F(i,4)=atr_v[i];
        F(i,5)=bb_width[i];
        F(i,6)=volr_v[i];
        F(i,7)=(close[i]-open[i])/std::max(1e-8,open[i]);
        F(i,8)=corr_v[i];
        F(i,9)=accel_v[i];
        F(i,10)=slope_v[i];
        F(i,11)=dist_sup[i];
        F(i,12)=dist_res[i];
        F(i,13)=flat_flag[i];
        F(i,14)=trend_up[i]-trend_dn[i];
        F(i,15)=vol_anom[i];
        F(i,16)=wick_ratio[i];
        F(i,17)=vol_price_div[i];
        F(i,18)=gap_jump[i];
        F(i,19)=manip_flag[i];
    }
    F.replace(arma::datum::nan,0.0);
    return F;
}

json make_features(const std::vector<double>& o,const std::vector<double>& h,const std::vector<double>& l,const std::vector<double>& c,const std::vector<double>& v){
    arma::Mat<double> raw(o.size(),6,arma::fill::zeros);
    for(size_t i=0;i<o.size();++i){raw(i,1)=o[i];raw(i,2)=h[i];raw(i,3)=l[i];raw(i,4)=c[i];raw(i,5)=v[i];}
    arma::Mat<double> F=build_feature_matrix(raw);
    json out; out["version"]=FEAT_VERSION; out["rows"]=F.n_rows; out["cols"]=F.n_cols; return out;
}

} // namespace etai
