#include <httplib.h>
#include "json.hpp"
#include <armadillo>
#include <fstream>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include "utils_data.h"
#include "features/features.h"
#include "server_accessors.h"

using namespace arma;
using nlohmann::json;

// ---------- helpers ----------
static inline double clampd(double v,double lo,double hi){
    if(!std::isfinite(v)) return lo;
    if(v<lo) return lo;
    if(v>hi) return hi;
    return v;
}
static inline bool env_enabled(const char* k){
    const char* s=getenv(k);
    return s && *s && (s[0]=='1'||s[0]=='T'||s[0]=='t'||s[0]=='Y'||s[0]=='y');
}
static inline double env_get(const char* k,double defv){
    const char* s=getenv(k);
    if(!s||!*s) return defv;
    try{ return std::stod(s);}catch(...){return defv;}
}
static inline double local_sharpe(const vec& pnl){
    if(pnl.n_rows==0) return 0.0;
    double mu=mean(pnl), sd=stddev(pnl,0);
    if(sd<=1e-9) return 0.0;
    return mu/sd;
}
static inline double local_maxdd(const vec& pnl){
    if(pnl.n_rows==0) return 0.0;
    vec eq=cumsum(pnl);
    double peak=eq(0), mdd=0.0;
    for(uword i=0;i<eq.n_rows;i++){
        peak=std::max(peak,eq(i));
        mdd=std::max(mdd,peak-eq(i));
    }
    return mdd;
}

// ---------- model load ----------
struct Policy {
    vec W;
    double b=0.0;
    vec mu;
    vec sd;
    int feat_dim=0;
    bool ok=false;
};
static Policy load_policy(const std::string& path){
    Policy p;
    try{
        std::ifstream f(path);
        if(!f.good()) return p;
        json j; f >> j;
        if(!j.contains("policy")) return p;
        const auto& pj=j["policy"];
        if(pj.contains("W")) p.W=vec(pj["W"].get<std::vector<double>>());
        if(pj.contains("b")){
            if(pj["b"].is_array() && pj["b"].size()>0)
                p.b=pj["b"][0].get<double>();
            else if(pj["b"].is_number()) p.b=pj["b"].get<double>();
        }
        if(pj.contains("feat_dim")) p.feat_dim=pj["feat_dim"].get<int>();
        if(pj.contains("norm")){
            const auto& n=pj["norm"];
            if(n.contains("mu")) p.mu=vec(n["mu"].get<std::vector<double>>());
            if(n.contains("sd")) p.sd=vec(n["sd"].get<std::vector<double>>());
        }
        p.ok=!p.W.is_empty();
    }catch(...){}
    return p;
}

// ---------- main route ----------
void register_train_env_routes(httplib::Server& svr){
    svr.Get("/api/train_env",[&](const httplib::Request& req, httplib::Response& res){
        json out; out["ok"]=false; out["env"]="v2";
        if(!env_enabled("ETAI_ENABLE_TRAIN_ENV")){
            out["error"]="not_enabled";
            res.set_content(out.dump(2),"application/json");
            return;
        }

        std::string symbol= req.has_param("symbol")?req.get_param_value("symbol"):"BTCUSDT";
        std::string interval=req.has_param("interval")?req.get_param_value("interval"):"15";
        int steps= req.has_param("steps")?std::stoi(req.get_param_value("steps")):3000;
        double fee=req.has_param("fee")?std::stod(req.get_param_value("fee")):0.0005;
        double tp=req.has_param("tp")?std::stod(req.get_param_value("tp")):0.003;
        double sl=req.has_param("sl")?std::stod(req.get_param_value("sl")):0.0018;
        std::string policy_name=req.has_param("policy")?req.get_param_value("policy"):"model";

        arma::mat raw;
        if(!etai::load_raw_ohlcv(symbol,interval,raw)){
            out["error"]="data_load_fail";
            res.set_content(out.dump(2),"application/json");
            return;
        }

        vec close=raw.col(4);
        vec fut(close.n_rows,fill::zeros);
        for(uword i=0;i+1<close.n_rows;i++)
            fut(i)=(close(i)>0.0)?(close(i+1)/close(i)-1.0):0.0;

        mat F=etai::build_feature_matrix(raw);
        if(F.n_rows==0){ out["error"]="feature_build_fail"; res.set_content(out.dump(2),"application/json"); return; }
        uword N=std::min<uword>(std::min(F.n_rows,fut.n_rows),(uword)steps);
        mat X=F.tail_rows(N);
        vec futN=fut.tail_rows(N);

        Policy pol=load_policy("cache/models/"+symbol+"_"+interval+"_ppo_pro.json");
        if(!pol.ok){ out["error"]="model_load_fail"; res.set_content(out.dump(2),"application/json"); return; }

        if(pol.feat_dim>0 && (int)X.n_cols>pol.feat_dim) X=X.cols(0,pol.feat_dim-1);
        if(pol.mu.n_rows==pol.sd.n_rows && pol.mu.n_rows==X.n_cols){
            for(uword j=0;j<X.n_cols;j++){
                double m=pol.mu(j), s=pol.sd(j);
                if(!std::isfinite(s)||s<1e-9) s=1.0;
                X.col(j)=(X.col(j)-m)/s;
            }
        }

        vec logits=X*pol.W + pol.b;
        double aggr=env_get("ETAI_AGGR_K",0.15);
        double elo=env_get("ETAI_CTX_E_LO",0.20);
        double thr=env_get("ETAI_THR",0.40);

        // сигмоид
        vec p=1.0/(1.0+exp(-logits));
        vec act(N,fill::zeros);
        for(uword i=0;i<N;i++){
            double s=(p(i)-0.5)*2.0*(1.0+aggr); // [-1,+1]
            if(std::fabs(s)<elo) act(i)=0.0;
            else if(s>=thr) act(i)=1.0;
            else if(s<=-thr) act(i)=-1.0;
            else act(i)=0.0;
        }

        bool use_atr=env_enabled("ETAI_ENV_ATR");
        vec pnl(N,fill::zeros);
        vec atr_col;
        if(F.n_cols>4){
            vec atr_all = abs(F.col(4));
            atr_col = atr_all.tail(N);
        } else {
            atr_col = vec(N,fill::zeros);
        }
        double k_atr=env_get("ETAI_ATR_K",0.35);

        for(uword i=0;i<N;i++){
            double frv=futN(i);
            double rr=0.0;
            double tp_e=tp, sl_e=sl;
            if(use_atr){
                double z=clampd(atr_col(i),0,1);
                tp_e=clampd(tp*(1.0+k_atr*z),1e-5,0.05);
                sl_e=clampd(sl*(1.0-k_atr*z*0.5),1e-5,0.05);
            }
            if(act(i)>0){
                if(frv>=tp_e) rr=tp_e;
                else if(frv<=-sl_e) rr=-sl_e;
                else rr=frv;
            }else if(act(i)<0){
                if(frv<=-sl_e) rr=tp_e;
                else if(frv>=tp_e) rr=-sl_e;
                else rr=-frv;
            }else rr=0.0;
            pnl(i)=rr-fee;
        }

        double sum_pos=0,sum_neg=0; int w=0,l=0;
        for(double v:pnl){ if(v>0){sum_pos+=v;w++;} else if(v<0){sum_neg+=v;l++;} }

        double pf=(sum_pos>0&&sum_neg<0)?(sum_pos/std::fabs(sum_neg)):0.0;
        double sharpe=local_sharpe(pnl);
        double dd=local_maxdd(pnl);
        double winrate=(w+l>0)?(double)w/(w+l):0.0;
        double eq_final=accu(pnl);

        out["ok"]=true;
        out["steps"]=(int)N;
        out["winrate"]=winrate;
        out["pf"]=pf;
        out["sharpe"]=sharpe;
        out["max_dd"]=dd;
        out["equity_final"]=eq_final;
        out["policy"]={{"name",policy_name},{"source","model_json"},{"thr",thr},{"feat_dim",(int)X.n_cols}};
        out["params"]={{"elo",elo},{"aggr",aggr},{"use_atr",use_atr}};
        res.set_content(out.dump(2),"application/json");
    });
}
