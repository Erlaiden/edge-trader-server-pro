#include <httplib.h>
#include "json.hpp"
#include <armadillo>
#include <cstdlib>
#include <string>
#include <algorithm>
#include <cmath>

#include "utils_data.h"
#include "features/features.h"
#include "rewardv2_accessors.h"   // etai::calc_sharpe, calc_max_drawdown, calc_winrate
#include "server_accessors.h"     // etai::{get_model_thr,get_model_ma_len,get_model_feat_dim}

using nlohmann::json;
using namespace arma;

// -------- helpers --------
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

// простая симуляция tp/sl/fee
static vec simulate_pnl(const vec& fut_ret, const vec& signal_long01,
                        double tp, double sl, double fee_abs)
{
    uword N = fut_ret.n_rows;
    vec r(N, fill::zeros);
    for(uword i=0;i<N;i++){
        double fr = fut_ret(i);
        bool is_long = (signal_long01(i) >= 0.5);
        double rr=0.0;
        if(is_long){
            if(fr>=tp)       rr =  tp;
            else if(fr<=-sl) rr = -sl;
            else             rr =  fr;
        }else{
            // short
            if(fr<=-sl)      rr =  tp;
            else if(fr>=tp)  rr = -sl;
            else             rr = -fr;
        }
        rr -= fee_abs;
        r(i)=rr;
    }
    return r;
}

// сигналы по политикам (без доступа к весам модели, всё самодостаточно)
static vec make_signals(const mat& F, const std::string& policy_name){
    uword N = F.n_rows;
    vec s(N, fill::zeros);

    // канал тренда: колонка 0 = (ema_fast - ema_slow)
    vec trend = F.n_cols>0? F.col(0) : vec(N,fill::zeros);

    if(policy_name=="thr_only" || policy_name=="model"){
        // proxy-вероятность из тренда (сигмоид от нормированного тренда)
        double scale = 1.0;
        if(F.n_cols>4){
            vec atr = abs(F.col(4));  // ATR-подобная
            double m = arma::mean(atr);
            if(std::isfinite(m) && m>1e-12) scale = 1.0/(3.0*m);
        }
        for(uword i=0;i<N;i++){
            double z = trend(i)*scale;
            double p = 1.0/(1.0+std::exp(-z)); // 0..1
            s(i) = p; // p>=0.5 => long
        }
    }else if(policy_name=="sign_channel"){
        // чистый знак тренда
        for(uword i=0;i<N;i++){
            s(i) = (trend(i)>=0.0)? 1.0: 0.0;
        }
    }else{
        // неизвестная политика -> фоллбэк
        for(uword i=0;i<N;i++){
            s(i) = (trend(i)>=0.0)? 1.0: 0.0;
        }
    }
    return s;
}

void register_train_env_routes(httplib::Server& svr){
    svr.Get("/api/train_env",[](const httplib::Request& req, httplib::Response& res){
        json out; out["ok"]=false; out["env"]="v1";

        if(!env_enabled("ETAI_ENABLE_TRAIN_ENV")){
            out["error"]="not_enabled";
            res.set_content(out.dump(2),"application/json");
            return;
        }

        // ---- параметры ----
        std::string symbol   = qs_str(req,"symbol","BTCUSDT");
        std::string interval = qs_str(req,"interval","15");
        int steps            = std::max(10, qs_int(req,"steps",3000));
        double fee           = std::max(0.0, qs_dbl(req,"fee",0.0005));
        double tp            = std::max(1e-4, qs_dbl(req,"tp",0.003));   // по умолчанию как в тренере
        double sl            = std::max(1e-4, qs_dbl(req,"sl",0.0018));
        std::string policy   = qs_str(req,"policy","model"); // "model" | "thr_only" | "sign_channel"

        // ---- данные ----
        arma::mat raw;
        if(!etai::load_raw_ohlcv(symbol,interval,raw)){
            out["error"]="data_load_fail";
            res.set_content(out.dump(2),"application/json");
            return;
        }
        // будущая доходность (ret_{t+1})
        vec close = raw.col(4);
        uword Nraw = raw.n_rows;
        vec fut(Nraw, fill::zeros);
        for(uword i=0;i+1<Nraw;i++){
            double c0=close(i), c1=close(i+1);
            fut(i) = (c0>0.0)? (c1/c0 - 1.0): 0.0;
        }
        fut(Nraw-1)=0.0;

        // ---- фичи ----
        mat F = etai::build_feature_matrix(raw);
        if(F.n_cols==0){
            out["error"]="feature_build_fail";
            res.set_content(out.dump(2),"application/json");
            return;
        }

        // выравнивание длины
        uword N = std::min<uword>(std::min(F.n_rows, fut.n_rows), (uword)steps);
        // берём последние N наблюдений
        mat Fw = F.tail_rows(N);
        vec fr = fut.tail_rows(N);

        // лёгкая адаптация размерности под текущую модель (мягко)
        int policy_dim = etai::get_model_feat_dim();
        bool feat_adapted = false;
        if((policy=="model") && policy_dim>0 && policy_dim!= (int)Fw.n_cols){
            if(Fw.n_cols > (uword)policy_dim){
                Fw = Fw.cols(0, policy_dim-1);
            }else{
                mat pad(Fw.n_rows, policy_dim, fill::zeros);
                pad.cols(0, Fw.n_cols-1) = Fw;
                Fw = std::move(pad);
            }
            feat_adapted = true;
        }

        // ---- сигналы по выбранной политике ----
        vec p_long01 = make_signals(Fw, policy);

        // ---- симуляция PnL ----
        vec pnl = simulate_pnl(fr, p_long01, tp, sl, fee);

        // ---- метрики ----
        double sum_pos = 0.0, sum_neg = 0.0;
        int wins=0, losses=0;
        for(uword i=0;i<pnl.n_rows;i++){
            if(pnl(i)>0){ sum_pos+=pnl(i); wins++; } else if(pnl(i)<0){ sum_neg+=pnl(i); losses++; }
        }
        double pf = (sum_pos>0 && sum_neg<0)? (sum_pos / std::fabs(sum_neg)) : 0.0;
        double sharpe = etai::calc_sharpe(pnl, 1e-12, 1.0);
        double dd_max = etai::calc_max_drawdown(pnl);
        double winrate = (wins+losses>0)? (double)wins/(wins+losses): 0.0;
        double equity_final = arma::accu(pnl);

        // ---- reward (PRO логика агрегата через аксессоры) ----
        double alpha_sh = etai::get_alpha_sharpe();
        double lam_risk = etai::get_lambda_risk();
        double mu_manip = etai::get_mu_manip(); // манипа у нас тут нет, оставим как 0
        double reward_v2 = (sum_pos - std::fabs(sum_neg))/std::max<uword>(pnl.n_rows,1) // profit avg
                           - lam_risk * dd_max
                           - mu_manip * 0.0
                           + alpha_sh * sharpe
                           - fee;

        // ---- ответ ----
        json gate; // на будущее: заполним нулями, чтобы не ломать фронт
        gate["mode"] = "soft";
        gate["checked"] = (int)N;
        gate["allowed"] = (int)N;
        gate["skipped"] = 0;

        out["ok"]=true;
        out["rows"] = (int)raw.n_rows;
        out["cols"] = (int)F.n_cols;
        out["steps"]= (int)N;
        out["feat_adapted"] = feat_adapted;
        out["fee"] = fee;
        out["tp"]  = tp;
        out["sl"]  = sl;

        out["policy"] = {
            {"name", policy},
            {"source", (policy=="model"?"model_json":"derived")},
            {"thr", etai::get_model_thr()},
            {"feat_dim", (policy=="model" && policy_dim>0)? policy_dim : (int)F.n_cols}
        };

        out["pf"] = pf;
        out["sharpe"] = sharpe;
        out["winrate"] = winrate;
        out["max_dd"] = dd_max;
        out["equity_final"] = equity_final;
        out["reward"] = reward_v2;
        out["wins"] = wins;
        out["losses"] = losses;
        out["gate"] = gate;

        res.set_content(out.dump(2),"application/json");
    });
}
