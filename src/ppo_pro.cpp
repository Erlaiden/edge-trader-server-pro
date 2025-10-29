// === ppo_pro.cpp (telemetry + ETAI_ACT_GATE + atomic save) ===
#include "ppo_pro.h"
#include "ppo.h"
#include "utils.h"
#include "optim/adam.h"
#include "features/features.h"
#include <armadillo>
#include <fstream>
#include <random>
#include <ctime>
#include <filesystem>
#include <cmath>
#include <algorithm>
#include <vector>
#include <cstdlib>
#include <cstring>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace etai {

static inline int get_env_int(const char* name, int defv) {
    const char* s = std::getenv(name);
    if (!s || !*s) return defv;
    return std::atoi(s);
}
static inline double get_env_double(const char* name, double defv) {
    const char* s = std::getenv(name);
    if (!s || !*s) return defv;
    return std::atof(s);
}
static inline std::string get_env_str(const char* name, const char* defv) {
    const char* s = std::getenv(name);
    return (s && *s) ? std::string(s) : std::string(defv);
}

// Simple rolling mean
static arma::vec rolling_mean(const arma::vec& x, int w) {
    arma::vec out(x.n_elem, arma::fill::zeros);
    double acc = 0.0;
    for (size_t i=0; i<x.n_elem; i++) {
        acc += x(i);
        if ((int)i >= w) acc -= x(i-w);
        if ((int)i >= w-1) out(i) = acc / w;
    }
    return out;
}
static arma::vec price_to_returns(const arma::vec& close) {
    arma::vec ret = arma::zeros(close.n_elem);
    for (size_t i=1; i<close.n_elem; i++)
        ret(i) = (close(i)-close(i-1))/close(i-1);
    return ret;
}
static inline double clamp(double v,double a,double b){return std::max(a,std::min(v,b));}

// === Network with independent optimizers ===
struct PolicyNet {
    arma::mat W; arma::vec b;
    Adam optW,optB;
    PolicyNet(int D,double lr):W(1,D),b(1),optW(lr),optB(lr){W=arma::randn(1,D)*0.01;b.zeros();}
    double forward(const arma::vec& x)const{return std::tanh(arma::as_scalar(W*x+b(0)));}
    void update(const arma::vec& x,double adv,double scale){
        arma::mat gW=adv*x.t(); arma::vec gB(1); gB(0)=adv;
        W=optW.step(W,gW*scale); b=optB.step(b,gB*scale);
    }
};
struct ValueNet {
    arma::mat W; arma::vec b;
    Adam optW,optB;
    ValueNet(int D,double lr):W(1,D),b(1),optW(lr),optB(lr){W=arma::randn(1,D)*0.01;b.zeros();}
    double forward(const arma::vec& x)const{return arma::as_scalar(W*x+b(0));}
    void update(const arma::vec& x,double target,double scale){
        double v=forward(x); double err=v-target;
        arma::mat gW=err*x.t(); arma::vec gB(1); gB(0)=err;
        W=optW.step(W,gW*scale); b=optB.step(b,gB*scale);
    }
};

// === PNL calc ===
static inline double nextbar_pnl(const arma::vec& c,const arma::vec& h,const arma::vec& l,
    size_t i,int sign,double tp,double sl){
    double entry=c(i);
    double tp_p=entry*(1.0+(sign>0?tp:-tp));
    double sl_p=entry*(1.0-(sign>0?sl:-sl));
    if ((sign>0&&l(i+1)<=sl_p)||(sign<0&&h(i+1)>=sl_p)) return -sl;
    if ((sign>0&&h(i+1)>=tp_p)||(sign<0&&l(i+1)<=tp_p)) return tp;
    double r=(c(i+1)-entry)/entry; return (sign>0)?r:-r;
}

// === Atomic save utility ===
static bool atomic_write_json(const std::string& path,const json& j){
    fs::path p(path); fs::create_directories(p.parent_path());
    std::string tmp=(path+".tmp");
    try{
        std::ofstream f(tmp,std::ios::trunc); if(!f.good()) return false;
        f<<j.dump(2); f.close();
        fs::rename(tmp,path);
        return true;
    }catch(...){return false;}
}

// === PRO training main ===
nlohmann::json trainPPO_pro(const arma::mat& M15,const arma::mat*,const arma::mat*,const arma::mat*,
    int episodes,double tp_init,double sl_init,int ma_init)
{
    if(M15.n_rows<5||M15.n_cols<200)
        return {{"ok",false},{"error","not_enough_data"}};

    const std::string MODE=get_env_str("ETAI_PRO_MODE","learn");
    const double ACT_GATE=get_env_double("ETAI_ACT_GATE",0.10);
    const int FOLDS=get_env_int("ETAI_CV_FOLDS",5);
    const long long ts_now=(long long)time(nullptr)*1000;

    const arma::vec close=M15.row(4).t();
    const arma::vec high =M15.row(2).t();
    const arma::vec low  =M15.row(3).t();

    arma::mat X=build_feature_matrix(M15);
    if(X.n_elem==0) return {{"ok",false},{"error","features_empty"}};
    X=arma::normalise(X);
    const size_t N=X.n_cols, warmup=32;
    if(N<=warmup+10) return {{"ok",false},{"error","too_short"}};

    int D=X.n_rows;
    PolicyNet pol(D,0.001); ValueNet val(D,0.001);

    double tp=tp_init, sl=sl_init;
    double total_r=0; int trades=0,wins=0; double dd=0,peak=0,equity=0;
    std::vector<double> eq; eq.reserve(N);

    for(size_t i=warmup;i+1<N-1;i++){
        arma::vec x=X.col(i);
        double a=pol.forward(x);
        if(std::abs(a)<ACT_GATE) continue;
        int sign=(a>=0)?+1:-1;
        double r=nextbar_pnl(close,high,low,i,sign,tp,sl);
        r=clamp(r,-sl,tp);
        double v=val.forward(x);
        double adv=r-v;
        pol.update(x,adv,1.0);
        val.update(x,r,1.0);
        equity+=r; peak=std::max(peak,equity); dd=std::max(dd,peak-equity);
        eq.push_back(equity);
        trades++; if(r>0)wins++; total_r+=r;
    }
    double acc=trades?(double)wins/trades:0;
    double mean=eq.empty()?0:eq.back()/(double)eq.size();
    double sd=0; for(double x:eq) sd+=(x-mean)*(x-mean);
    sd=(eq.size()>1)?sqrt(sd/(eq.size()-1)):1;
    double sh=mean/sd;

    json meta{
        {"ok",true},
        {"version",3},
        {"schema","ppo_pro_v1"},
        {"build_ts",ts_now},
        {"tp",tp},{"sl",sl},{"ma_len",ma_init},
        {"best_thr",0.0006},
        {"act_gate",ACT_GATE},
        {"oos_summary",{
            {"reward",total_r},{"trades",trades},{"accuracy",acc},
            {"sharpe",sh},{"drawdown_max",dd},{"expectancy",trades?total_r/trades:0.0}
        }},
        {"policy",{
            {"feat_dim",D},{"feat_version",1},
            {"W",std::vector<double>(pol.W.begin(),pol.W.end())},
            {"b",std::vector<double>(pol.b.begin(),pol.b.end())}
        }},
        {"policy_source","learn"},
        {"path","cache/models/BTCUSDT_15_ppo_pro.json"}
    };
    atomic_write_json("cache/models/BTCUSDT_15_ppo_pro.json",meta);
    std::ofstream f("cache/logs/last_train_telemetry.json");
    f<<json{{"ts",ts_now},{"reward",total_r},{"sharpe",sh},{"expectancy",trades?total_r/trades:0.0},
            {"accuracy",acc},{"drawdown_max",dd},{"trades",trades}}.dump(2);
    f.close();
    return meta;
}

} // namespace etai
