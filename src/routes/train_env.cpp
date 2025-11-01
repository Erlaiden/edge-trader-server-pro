#include <httplib.h>
#include "json.hpp"
#include <armadillo>
#include <cstdlib>
#include <string>
#include <algorithm>
#include <cmath>

#include "utils_data.h"
#include "features/features.h"
#include "env/env_trading.h"
#include "env/episode_runner.h"
#include "env/reward_live.h"

using nlohmann::json;

// ===== helper функции =====
static inline bool env_enabled(const char* k){
    const char* s = std::getenv(k); if(!s||!*s) return false;
    return (s[0]=='1')||(s[0]=='T'||s[0]=='t')||(s[0]=='Y'||s[0]=='y');
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

// ===== основной роут =====
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
        int steps            = qs_int(req,"steps",3000);
        double fee           = std::max(0.0, qs_dbl(req,"fee",0.0005));
        std::string policy   = qs_str(req,"policy","model");

        arma::mat raw;
        if(!etai::load_raw_ohlcv(symbol,interval,raw)){
            out["error"]="data_load_fail";
            res.set_content(out.dump(2),"application/json");
            return;
        }

        arma::mat F = etai::build_feature_matrix(raw);
        if(F.n_cols==0){
            out["error"]="feature_build_fail";
            res.set_content(out.dump(2),"application/json");
            return;
        }

        const int feat_dim = (int)F.n_cols;
        const int policy_dim = etai::get_model_feat_dim();
        bool feat_adapted = false;
        arma::mat F_used = F;

        if(policy=="model" && policy_dim>0 && policy_dim!=feat_dim){
            if(feat_dim > policy_dim)
                F_used = F.cols(0, policy_dim-1);
            else{
                arma::mat pad(F.n_rows, policy_dim, arma::fill::zeros);
                pad.cols(0, feat_dim-1)=F;
                F_used=std::move(pad);
            }
            feat_adapted=true;
        }

        double model_thr = etai::get_model_thr();
        int model_ma     = etai::get_model_ma_len();

        etai::EnvConfig cfg;
        cfg.fee_per_trade = fee;
        cfg.model_thr = model_thr;
        cfg.model_ma_len = model_ma;
        cfg.policy_name = policy;
        cfg.max_steps = std::max(10, steps);

        etai::EpisodeStats st = etai::run_episode(F_used, raw, cfg);

        double pf      = st.profit_factor;
        double sharpe  = st.sharpe;
        double winrate = st.winrate;
        double max_dd  = st.max_drawdown;
        int trades     = st.trades;
        double equity  = st.equity_final;

        etai::RewardParams rp;
        double R = etai::reward_live(pf, sharpe, winrate, max_dd, fee, trades, rp);

        out["ok"]=true;
        out["steps"]=cfg.max_steps;
        out["feat_adapted"]=feat_adapted;
        out["fee"]=fee;
        out["policy"]={
            {"name",policy},
            {"source",(policy=="model"?"model_json":"derived")},
            {"thr",model_thr},
            {"feat_dim",(policy=="model"?policy_dim:feat_dim)}
        };
        out["trades"]=trades;
        out["equity_final"]=equity;
        out["pf"]=pf;
        out["sharpe"]=sharpe;
        out["winrate"]=winrate;
        out["max_dd"]=max_dd;
        out["reward"]=R;

        // минимальная телеметрия HTF/гейта (если есть)
        json htf;
        htf["agree240"]=st.htf.agree240;
        htf["agree1440"]=st.htf.agree1440;
        out["htf"]=htf;

        json gate;
        gate["mode"]=st.gate.mode;
        gate["checked"]=st.gate.checked;
        gate["allowed"]=st.gate.allowed;
        gate["skipped"]=st.gate.skipped;
        out["gate"]=gate;

        res.set_content(out.dump(2),"application/json");
    });
}
