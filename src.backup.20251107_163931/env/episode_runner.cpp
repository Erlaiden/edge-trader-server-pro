#include <armadillo>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include "../features/features.h"
#include "../utils_data.h"
#include "json.hpp"

using namespace arma;
using json = nlohmann::json;

// --- helpers ---
static inline double clampd(double v,double lo,double hi){
    if(!std::isfinite(v)) return lo;
    if(v<lo) return lo;
    if(v>hi) return hi;
    return v;
}
static inline double env_get(const char* k,double defv){
    const char* s = getenv(k);
    if(!s||!*s) return defv;
    try{ return std::stod(s); }catch(...){ return defv; }
}

// --- core logic ---
namespace etai {

struct EpisodeMetrics {
    double winrate=0.0, pf=0.0, sharpe=0.0, max_dd=0.0, equity_final=0.0;
};

static double local_sharpe(const vec& pnl){
    if(pnl.n_rows==0) return 0.0;
    double mu=mean(pnl), sd=stddev(pnl,0);
    if(sd<=1e-9) return 0.0;
    return mu/sd;
}
static double local_maxdd(const vec& pnl){
    if(pnl.n_rows==0) return 0.0;
    vec eq=cumsum(pnl);
    double peak=eq(0), mdd=0.0;
    for(uword i=0;i<eq.n_rows;i++){
        peak=std::max(peak,eq(i));
        mdd=std::max(mdd,peak-eq(i));
    }
    return mdd;
}

// --- simple decision function with gates ---
static vec decide_actions(const vec& score, double thr, double elo, double aggr){
    uword N=score.n_rows;
    vec act(N,fill::zeros);
    for(uword i=0;i<N;i++){
        double s = clampd(score(i),-1.0,1.0);
        double s_adj = s*(1.0+aggr); // агрессия повышает чувствительность
        if(std::fabs(s_adj) < elo){ act(i)=0.0; continue; }
        if(s_adj >= thr) act(i)=+1.0;
        else if(s_adj <= -thr) act(i)=-1.0;
        else act(i)=0.0;
    }
    return act;
}

// --- simulate one run ---
EpisodeMetrics run_episode(const mat& feats, const vec& fut_ret, double thr, double elo, double aggr, double tp, double sl, double fee){
    uword N = std::min(fut_ret.n_rows, feats.n_rows);
    vec score = feats.col(0); // простая замена реального policy output
    vec act = decide_actions(score,thr,elo,aggr);

    vec pnl(N,fill::zeros);
    for(uword i=0;i<N;i++){
        double fr=fut_ret(i);
        double rr=0.0;
        if(act(i)>0){ // long
            if(fr>=tp) rr=tp;
            else if(fr<=-sl) rr=-sl;
            else rr=fr;
        }else if(act(i)<0){ // short
            if(fr<=-sl) rr=tp;
            else if(fr>=tp) rr=-sl;
            else rr=-fr;
        }else rr=0.0;
        pnl(i)=rr-fee;
    }

    double sum_pos=0,sum_neg=0; int w=0,l=0;
    for(double v:pnl){
        if(v>0){sum_pos+=v;w++;}
        else if(v<0){sum_neg+=v;l++;}
    }

    EpisodeMetrics M;
    M.winrate = (w+l>0)? (double)w/(w+l):0.0;
    M.pf = (sum_pos>0 && sum_neg<0)? (sum_pos/std::fabs(sum_neg)):0.0;
    M.sharpe = local_sharpe(pnl);
    M.max_dd = local_maxdd(pnl);
    M.equity_final = arma::accu(pnl);
    return M;
}

json run_episode_json(const mat& feats, const vec& fut_ret, double tp, double sl, double fee){
    double thr  = env_get("ETAI_THR", 0.4);
    double elo  = env_get("ETAI_CTX_E_LO", 0.2);
    double aggr = env_get("ETAI_AGGR_K", 0.15);
    EpisodeMetrics M = run_episode(feats,fut_ret,thr,elo,aggr,tp,sl,fee);
    json j;
    j["ok"]=true;
    j["thr"]=thr; j["elo"]=elo; j["aggr"]=aggr;
    j["winrate"]=M.winrate;
    j["pf"]=M.pf;
    j["sharpe"]=M.sharpe;
    j["max_dd"]=M.max_dd;
    j["equity_final"]=M.equity_final;
    return j;
}

} // namespace etai
