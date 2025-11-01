#include <httplib.h>
#include "json.hpp"
#include <armadillo>
#include <cstdlib>
#include <string>
#include <algorithm>
#include <fstream>
#include <cmath>
#include "utils_data.h"
#include "features/features.h"
#include "server_accessors.h"   // get_model_thr,get_model_ma_len,get_model_feat_dim

using nlohmann::json;
using namespace arma;

// ---- utils ----
static inline bool env_enabled(const char* k){
    const char* s=getenv(k);
    return s && *s && (s[0]=='1'||s[0]=='T'||s[0]=='t'||s[0]=='Y'||s[0]=='y');
}
static inline int qs_int(const httplib::Request& r, const char* k, int defv){
    try{ return r.has_param(k)? std::stoi(r.get_param_value(k)):defv; }catch(...){ return defv; }
}
static inline double qs_dbl(const httplib::Request& r, const char* k, double defv){
    try{ return r.has_param(k)? std::stod(r.get_param_value(k)):defv; }catch(...){ return defv; }
}
static inline std::string qs_str(const httplib::Request& r, const char* k, const char* defv){
    return r.has_param(k)? r.get_param_value(k): std::string(defv);
}
static inline double clampd(double v,double lo,double hi){
    if(!std::isfinite(v)) return lo; if(v<lo) return lo; if(v>hi) return hi; return v;
}

// ---- local metrics (no external deps) ----
static double local_sharpe(const vec& pnl, double eps=1e-12, double scale=1.0){
    if(pnl.n_rows==0) return 0.0;
    double mu=mean(pnl); double sd=stddev(pnl,0);
    if(!std::isfinite(sd) || sd<eps) return 0.0;
    return scale * (mu/(sd+eps));
}
static double local_max_drawdown(const vec& pnl){
    if(pnl.n_rows==0) return 0.0;
    vec eq=cumsum(pnl); double peak=eq(0), maxdd=0.0;
    for(uword i=0;i<eq.n_rows;i++){ peak=std::max(peak, eq(i)); maxdd=std::max(maxdd, peak-eq(i)); }
    return maxdd;
}

// ---- load model W,b from JSON ----
struct ModelWB { std::vector<double> W; double b=0.0; int feat_dim=0; bool ok=false; };
static ModelWB load_model_wb(const std::string& path){
    ModelWB m;
    try{
        std::ifstream f(path); if(!f.good()) return m;
        json j; f >> j;
        if(!j.contains("policy")) return m;
        const auto& p = j["policy"];
        if(p.contains("W") && p["W"].is_array())      m.W = p["W"].get<std::vector<double>>();
        if(p.contains("b") && p["b"].is_array() && p["b"].size()>0) m.b = p["b"][0].get<double>();
        else if(p.contains("b") && p["b"].is_number())             m.b = p["b"].get<double>();
        if(p.contains("feat_dim")) m.feat_dim = p["feat_dim"].get<int>();
        m.ok = !m.W.empty();
    }catch(...) {}
    return m;
}

// simple Z-score by train slice (proxy; real training stats недоступны)
static void zscore_by_ref(mat& X, const mat& ref){
    if(X.n_rows==0||X.n_cols==0||ref.n_rows==0) return;
    vec mu = arma::mean(ref,0).t(); vec sd = arma::stddev(ref,0,0).t();
    for(uword j=0;j<X.n_cols;j++){
        double s=(std::isfinite(sd(j)) && sd(j)>1e-12)? sd(j):1.0;
        X.col(j) = (X.col(j)-mu(j))/s;
    }
}

// ATR proxy (col 4 в фичах — наш ATR)
static vec atr_col(const mat& F){ if(F.n_cols>4) return abs(F.col(4)); return vec(F.n_rows,fill::zeros); }

// Энергия из ATR в [0..1] (робастная нормировка через медиану)
static vec energy01_from_atr(const vec& atr){
    vec a = atr;
    if(a.n_rows==0) return a;
    double med = arma::median(a);
    if(!std::isfinite(med) || med<=1e-12) med = 1e-3;
    vec e = a / (3.0*med);            // ~1 при ~3*median
    for(uword i=0;i<e.n_rows;i++){
        double v = e(i); if(!std::isfinite(v)) v=0.0;
        e(i) = std::max(0.0, std::min(1.0, v));
    }
    return e;
}

// aggressive bias (логит-сдвиг по тренду; ограничен)
static void apply_aggr_bias(vec& logits, const mat& F){
    double k = getenv("ETAI_AGGR_K")? std::atof(getenv("ETAI_AGGR_K")): 0.15;
    if(!std::isfinite(k) || k<0) k=0.15;
    vec trend = F.n_cols>0? F.col(0): vec(F.n_rows,fill::zeros);
    for(uword i=0;i<logits.n_rows;i++){
        logits(i) = clampd(logits(i) + k*trend(i), -0.25, 0.25);
    }
}

static vec sigmoid_vec(const vec& z){ vec p=z; for(uword i=0;i<p.n_rows;i++){ double t=p(i); p(i)=1.0/(1.0+std::exp(-t)); } return p; }

// Пороговая симуляция c THR и E_LO-гейтингом
static vec simulate_pnl_thr(const vec& fut_ret, const vec& p_long01,
                            const vec& atr, const vec& energy01,
                            double tp, double sl, double fee_abs,
                            bool atr_scale, double thr_cut, double e_lo,
                            unsigned& skipped_out)
{
    uword N=fut_ret.n_rows; vec r(N,fill::zeros);
    skipped_out = 0u;
    double k_atr = getenv("ETAI_ATR_K")? std::atof(getenv("ETAI_ATR_K")): 0.35;
    for(uword i=0;i<N;i++){
        // Гейтинг по энергии
        if(i<energy01.n_rows && energy01(i) < e_lo){
            // сделку не открываем — считаем скип
            skipped_out++;
            continue;
        }

        double tp_e=tp, sl_e=sl;
        if(atr_scale && i<atr.n_rows){
            double z = atr(i);
            tp_e = clampd(tp*(1.0 + k_atr*z), 1e-5, 0.05);
            sl_e = clampd(sl*(1.0 - k_atr*z*0.5), 1e-5, 0.05);
        }
        double fr = fut_ret(i);
        bool is_long = (p_long01(i) >= thr_cut);
        double rr = 0.0;
        if(is_long){
            if(fr>=tp_e) rr= tp_e;
            else if(fr<=-sl_e) rr=-sl_e;
            else rr= fr;
        }else{
            if(fr<=-sl_e) rr= tp_e;
            else if(fr>= tp_e) rr=-sl_e;
            else rr=-fr;
        }
        rr -= fee_abs;
        r(i)=rr;
    }
    return r;
}

// policy: real model (W,b) or simple thresholds
static vec make_signals(const mat& F, const std::string& policy_name){
    uword N=F.n_rows; vec s(N,fill::zeros);
    if(policy_name=="model"){
        ModelWB m = load_model_wb("cache/models/BTCUSDT_15_ppo_pro.json");
        if(!m.ok){
            vec trend = F.n_cols>0? F.col(0): vec(N,fill::zeros);
            vec logits = trend;
            logits.for_each([](double& v){ v *= 1.0; });
            apply_aggr_bias(logits, F);
            return sigmoid_vec(logits);
        }
        mat X = F;
        if((int)X.n_cols > m.feat_dim && m.feat_dim>0) X = X.cols(0, m.feat_dim-1);
        if((int)X.n_cols < m.feat_dim && m.feat_dim>0){
            mat pad(X.n_rows, m.feat_dim, fill::zeros); pad.cols(0, X.n_cols-1)=X; X=std::move(pad);
        }
        uword split = std::max<uword>(1, (uword)(X.n_rows*0.5));
        mat ref = X.rows(0, split-1);
        zscore_by_ref(X, ref);
        vec logits = X * vec(m.W);
        logits += m.b;
        apply_aggr_bias(logits, X);
        return sigmoid_vec(logits);
    }else{
        vec trend = F.n_cols>0? F.col(0): vec(N,fill::zeros);
        vec logits = trend;
        apply_aggr_bias(logits, F);
        return sigmoid_vec(logits);
    }
}

// ---- main route ----
void register_train_env_routes(httplib::Server& svr){
    svr.Get("/api/train_env",[](const httplib::Request& req, httplib::Response& res){
        json out; out["ok"]=false; out["env"]="v1";
        if(!env_enabled("ETAI_ENABLE_TRAIN_ENV")){
            out["error"]="not_enabled";
            res.set_content(out.dump(2),"application/json");
            return;
        }
        std::string symbol   = qs_str(req,"symbol","BTCUSDT");
        std::string interval = qs_str(req,"interval","15");
        int    steps         = std::max(10, qs_int(req,"steps",3000));
        double fee           = std::max(0.0, qs_dbl(req,"fee",0.0005));
        double tp            = std::max(1e-5, qs_dbl(req,"tp",0.003));
        double sl            = std::max(1e-5, qs_dbl(req,"sl",0.0018));
        std::string policy   = qs_str(req,"policy","model"); // model | thr_only

        // --- overrides via URL OR ENV ---
        // THR
        double thr_cut = qs_dbl(req,"thr",
                           std::getenv("ETAI_THR")? std::atof(getenv("ETAI_THR")) : 0.40);
        thr_cut = clampd(thr_cut, 0.01, 0.99);

        // E_LO (контекст энергии)
        double e_lo = qs_dbl(req,"elo",
                         std::getenv("ETAI_CTX_E_LO")? std::atof(getenv("ETAI_CTX_E_LO")) : 0.20);
        e_lo = clampd(e_lo, 0.0, 1.0);

        // AGGR bias k
        double aggr = qs_dbl(req,"aggr",
                         std::getenv("ETAI_AGGR_K")? std::atof(getenv("ETAI_AGGR_K")) : 0.15);
        if(!std::isfinite(aggr) || aggr<0) aggr=0.15;
        setenv("ETAI_AGGR_K",(std::to_string(aggr)).c_str(),1);

        // ATR on/off
        bool use_atr = qs_int(req,"atr", env_enabled("ETAI_ENV_ATR")?1:0) != 0;

        arma::mat raw;
        if(!etai::load_raw_ohlcv(symbol,interval,raw)){
            out["error"]="data_load_fail";
            res.set_content(out.dump(2),"application/json");
            return;
        }

        // future return (t+1)
        vec close = raw.col(4);
        uword Nraw = raw.n_rows;
        vec fut(Nraw, fill::zeros);
        for(uword i=0;i+1<Nraw;i++){
            double c0=close(i), c1=close(i+1);
            fut(i)=(c0>0.0)? (c1/c0-1.0):0.0;
        }
        fut(Nraw-1)=0.0;

        // features
        mat F = etai::build_feature_matrix(raw);
        if(F.n_cols==0){
            out["error"]="feature_build_fail";
            res.set_content(out.dump(2),"application/json");
            return;
        }

        // align length
        uword N = std::min<uword>(std::min(F.n_rows, fut.n_rows), (uword)steps);
        mat Fw = F.tail_rows(N);
        vec fr = fut.tail_rows(N);

        // signals
        vec p_long01 = make_signals(Fw, policy);

        // energy + atr
        vec atrv = atr_col(Fw);
        vec e01  = energy01_from_atr(atrv);

        // pnl simulation with THR + E_LO gating
        unsigned skipped=0u;
        vec pnl = simulate_pnl_thr(fr, p_long01, atrv, e01, tp, sl, fee, use_atr, thr_cut, e_lo, skipped);

        // metrics
        double sum_pos=0.0, sum_neg=0.0; int wins=0, losses=0;
        for(uword i=0;i<pnl.n_rows;i++){ if(pnl(i)>0){sum_pos+=pnl(i); wins++;} else if(pnl(i)<0){sum_neg+=pnl(i); losses++;} }
        double pf = (sum_pos>0 && sum_neg<0)? (sum_pos/std::fabs(sum_neg)) : 0.0;
        double sharpe = local_sharpe(pnl, 1e-12, 1.0);
        double dd_max = local_max_drawdown(pnl);
        double winrate = (wins+losses>0)? (double)wins/(wins+losses):0.0;
        double equity_final = arma::accu(pnl);

        // params echo
        json params = {
            {"thr", thr_cut},
            {"e_lo", e_lo},
            {"aggr", aggr},
            {"use_atr", use_atr}
        };

        out["ok"]=true;
        out["rows"]=(int)raw.n_rows; out["cols"]=(int)F.n_cols; out["steps"]=(int)N;
        out["fee"]=fee; out["tp"]=tp; out["sl"]=sl; out["use_atr"]=use_atr;
        out["policy"]={{"name",policy},{"source",(policy=="model"?"model_json":"derived")},
                       {"thr", thr_cut},{"feat_dim",(int)Fw.n_cols}};
        out["pf"]=pf; out["sharpe"]=sharpe; out["winrate"]=winrate; out["max_dd"]=dd_max; out["equity_final"]=equity_final;
        out["wins"]=wins; out["losses"]=losses;
        out["skipped"]=(int)skipped;
        out["params"]=params;

        res.set_content(out.dump(2),"application/json");
    });
}
