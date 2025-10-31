#include "ppo_pro.h"
#include "features.h"
#include "json.hpp"
#include <armadillo>
#include <vector>
#include <cmath>
#include <stdexcept>
#include <iostream>
#include <cstdlib>

#include "metrics.h"
#include "rewardv2_accessors.h"

#include "features/support_resistance.h"
#include "features/manip_detector.h"

using nlohmann::json;
using namespace arma;

namespace etai {

arma::Mat<double> build_feature_matrix(const arma::Mat<double>&);

// ----------------- утилиты -----------------
static inline bool env_enabled(const char* k){
    const char* s = std::getenv(k);
    if(!s || !*s) return false;
    return (s[0]=='1') || (s[0]=='T'||s[0]=='t') || (s[0]=='Y'||s[0]=='y');
}
static inline double clampd(double v,double lo,double hi){
    if(!std::isfinite(v)) return lo;
    if(v<lo) return lo;
    if(v>hi) return hi;
    return v;
}
static inline double sigmoid(double z){
    if(!std::isfinite(z)) z=0.0;
    return 1.0/(1.0+std::exp(-z));
}

// простая логрег
static void train_logreg(const mat& X,const vec& y,vec& W,double& b,int epochs,double lr,double l2){
    W.set_size(X.n_cols); W.zeros(); b=0.0;
    for(int e=0;e<epochs;++e){
        vec z=X*W+b;
        vec p=1.0/(1.0+arma::exp(-z));
        vec g=X.t()*(p-y)/X.n_rows + l2*W;
        double gb=arma::accu(p-y)/X.n_rows;
        W-=lr*g; b-=lr*gb;
    }
}
static vec predict_proba(const mat& X,const vec& W,double b){
    vec z=X*W+b;
    for(uword i=0;i<z.n_rows;++i) z(i)=sigmoid(z(i));
    return z;
}

// PnL серия под tp/sl + комиссия
static vec pnl_series(const vec& fut_ret, const vec& proba, double thr, double tp, double sl, double fee_abs){
    const uword N=fut_ret.n_rows;
    vec r(N, fill::zeros);
    for(uword i=0;i<N;++i){
        double fr=fut_ret(i), pr=proba(i);
        if(pr>=thr){
            if(fr>=tp)       r(i) =  tp;
            else if(fr<=-sl) r(i) = -sl;
            else             r(i) =  fr;
            r(i) -= fee_abs;
        }else{
            if(fr<=-sl)      r(i) =  tp;
            else if(fr>=tp)  r(i) = -sl;
            else             r(i) = -fr;
            r(i) -= fee_abs;
        }
    }
    return r;
}

// простая симуляция Rv1 — только для поиска best_thr
static double simulate_reward_v1(const vec& fut_ret,const vec& proba,double thr,double tp,double sl){
    double R=0.0; const uword N=fut_ret.n_rows;
    for(uword i=0;i<N;++i){
        double fr=fut_ret(i), pr=proba(i);
        if(pr>=thr){
            if(fr>=tp)       R += tp;
            else if(fr<=-sl) R -= sl;
            else             R += fr;
        }else{
            if(fr<=-sl)      R += tp;
            else if(fr>=tp)  R -= sl;
            else             R -= fr;
        }
    }
    return R;
}

// знак «тренда» из фич: берем колонку 0 (ema_fast - ema_slow) и усредняем
static int trend_sign_from_features(const mat& F, uword start_row, uword end_row){
    if (F.n_rows==0 || F.n_cols==0) return 0;
    if (start_row>=F.n_rows) start_row = F.n_rows-1;
    if (end_row>=F.n_rows)   end_row   = F.n_rows-1;
    if (end_row < start_row) std::swap(start_row, end_row);
    vec col = F.col(0).rows(start_row, end_row);
    double m = arma::mean(col);
    if (m> 1e-12) return +1;
    if (m<-1e-12) return -1;
    return 0;
}

// ---------- Тренер ----------
json trainPPO_pro(const arma::mat& raw15,
                  const arma::mat* raw60,
                  const arma::mat* raw240,
                  const arma::mat* raw1440,
                  int /*episodes*/,
                  double tp, double sl, int /*ma_len*/,
                  bool /*use_antimanip*/)
{
    json out=json::object();
    try{
        if(raw15.n_cols<6||raw15.n_rows<300){
            out["ok"]=false; out["error"]="bad_raw_shape";
            out["raw_cols"]=(int)raw15.n_cols; out["N_rows"]=(int)raw15.n_rows;
            return out;
        }

        // 1) Фичи 15m
        mat F = build_feature_matrix(raw15);
        const uword N = F.n_rows;
        const uword D = F.n_cols;
        const int FEAT_VERSION = (D > 28 ? 10 : 9);

        // 2) Будущая доходность
        vec close = raw15.col(4);
        vec high  = raw15.col(2);
        vec low   = raw15.col(3);
        vec open  = raw15.col(1);

        vec r(N, fill::zeros);
        for(uword i=0;i+1<N;++i){
            double c0=close(i), c1=close(i+1);
            r(i) = (c0>0.0) ? (c1/c0 - 1.0) : 0.0;
        }
        vec fut = arma::shift(r,-1); fut(N-1)=0.0;

        // 3) Разметка
        double thr_pos = clampd(tp, 1e-4, 1e-1);
        double thr_neg = clampd(sl, 1e-4, 1e-1);
        std::vector<uword> idx; idx.reserve(N);
        for(uword i=0;i<N;++i){
            double fr=fut(i);
            if(fr>=thr_pos || fr<=-thr_neg) idx.push_back(i);
        }
        const uword M = (uword)idx.size();
        if(M<200){
            out["ok"]=false; out["error"]="not_enough_labeled";
            out["M_labeled"]=(int)M; out["N_rows"]=(int)N;
            out["raw_cols"] =(int)raw15.n_cols; out["feat_cols"]=(int)D;
            return out;
        }

        mat Xs(M, D, fill::zeros);
        vec ys(M, fill::zeros);
        vec fut_s(M, fill::zeros);
        for(uword k=0;k<M;++k){
            uword i = idx[k];
            Xs.row(k) = F.row(i);
            ys(k)     = (fut(i) >= thr_pos) ? 1.0 : 0.0;
            fut_s(k)  = fut(i);
        }
        uword split = (uword)std::floor(M*0.8);
        if(split==0 || split>=M) split = M>1 ? M-1 : 1;

        mat Xtr = Xs.rows(0, split-1);  vec ytr = ys.rows(0, split-1);
        mat Xva = Xs.rows(split, M-1);  vec yva = ys.rows(split, M-1);
        vec fr_va = fut_s.rows(split, M-1);

        // 4) Z-score нормализация (по колонкам трейна)
        vec mu = arma::mean(Xtr, 0).t();
        vec sd = arma::stddev(Xtr, 0, 0).t();
        for(uword j=0;j<D;++j){
            double s = (std::isfinite(sd(j)) && sd(j)>1e-12) ? sd(j) : 1.0;
            Xtr.col(j) = (Xtr.col(j) - mu(j)) / s;
            Xva.col(j) = (Xva.col(j) - mu(j)) / s;
        }

        // 5) Логрег
        vec W; double b=0.0;
        train_logreg(Xtr, ytr, W, b, /*epochs*/300, /*lr*/0.05, /*l2*/1e-4);
        vec pv = predict_proba(Xva, W, b);

        // 6) Accuracy@0.5
        vec pred01 = conv_to<vec>::from(pv >= 0.5);
        double acc = arma::mean( conv_to<vec>::from(pred01 == yva) );

        // 7) Поиск best_thr по v1
        double best_thr=0.50, best_Rv1=-1e100;
        for(double thr=0.30; thr<=0.70+1e-12; thr+=0.02){
            double R = simulate_reward_v1(fr_va, pv, thr, thr_pos, thr_neg);
            if(R > best_Rv1){ best_Rv1 = R; best_thr = thr; }
        }
        best_thr = clampd(best_thr, 1e-4, 0.99);

        // 8) Anti-manip (за флагом)
        double manip_ratio = 0.0;
        if(env_enabled("ETAI_ENABLE_ANTI_MANIP")){
            // серии
            std::vector<double> Vopen(N), Vhigh(N), Vlow(N), Vclose(N);
            for(uword i=0;i<N;++i){ Vopen[i]=open(i); Vhigh[i]=high(i); Vlow[i]=low(i); Vclose[i]=close(i); }

            auto sup = rolling_support(Vlow,  20);
            auto res = rolling_resistance(Vhigh,20);
            auto fbf = false_break_flags(Vopen, Vhigh, Vlow, Vclose, sup, res, /*tol*/5e-4);

            // считаем только на валидации по индексам разметки
            uword a = std::min<uword>(split, (uword)idx.size());
            uword bnd = (idx.size()==0) ? 0 : (uword)idx.size();
            uword cnt=0, den=0;
            for(uword k=a; k<bnd; ++k){
                uword i = idx[k];
                if(i < fbf.size()){
                    ++den;
                    if(fbf[i]!=0) ++cnt;
                }
            }
            manip_ratio = (den>0)? (double)cnt/(double)den : 0.0;
        }

        // 9) Reward v2 @best_thr (база)
        double fee  = etai::get_fee_per_trade();
        double a_sh = etai::get_alpha_sharpe();
        double lam  = etai::get_lambda_risk();
        double mu_m = etai::get_mu_manip();

        vec pnl = pnl_series(fr_va, pv, best_thr, thr_pos, thr_neg, fee);
        double sharpe   = etai::calc_sharpe(pnl, 1e-12, 1.0);
        double dd_max   = etai::calc_max_drawdown(pnl);
        double winrate  = etai::calc_winrate(pnl);
        double profit   = arma::accu(pnl) / std::max<arma::uword>(pnl.n_elem,1);
        double risk     = dd_max;
        double reward_v2= profit - lam*risk - mu_m*manip_ratio + a_sh*sharpe - fee;

        // 10) Мягкий MTF-контекст (под флагом), даёт множитель wctx_htf ∈ [0.90..1.05]
        double wctx_htf = 1.0;
        int htf_agree60 = 0, htf_agree240 = 0;
        if (env_enabled("ETAI_MTF_ENABLE")){
            // считаем «знак тренда» 15m на валидационной части
            uword i0 = idx[(split>0? split:0)];
            uword i1 = idx[M-1];
            int s15 = trend_sign_from_features(F, i0, i1);

            auto sign_from_raw = [&](const arma::mat* raw)->int{
                if(!raw || raw->n_rows<10 || raw->n_cols<6) return 0;
                arma::mat Fh = build_feature_matrix(*raw);
                if (Fh.n_cols==0) return 0;
                return trend_sign_from_features(Fh, (uword)0, Fh.n_rows>20? (uword)(Fh.n_rows-1): (uword)(Fh.n_rows-1));
            };

            int s60   = sign_from_raw(raw60);
            int s240  = sign_from_raw(raw240);

            // согласие: +1 если совпали знаки, -1 если разные, 0 если нули
            auto agree = [](int a,int b)->int{ if(a==0||b==0) return 0; return (a==b)? +1 : -1; };
            htf_agree60  = agree(s15, s60);
            htf_agree240 = agree(s15, s240);

            // небольшие веса: 60m ±2%, 240m ±3%
            wctx_htf = 1.0 + 0.02*htf_agree60 + 0.03*htf_agree240;
            wctx_htf = clampd(wctx_htf, 0.90, 1.05);
        }

        double reward_wctx = reward_v2 * wctx_htf;

        // 11) Политика + метрики
        json policy;
        policy["W"]            = std::vector<double>(W.begin(), W.end());
        policy["b"]            = { b };
        policy["feat_dim"]     = (int)W.n_rows;
        policy["feat_version"] = FEAT_VERSION;
        policy["note"]         = "logreg_v2_reward";

        json metrics;
        metrics["val_accuracy"]   = acc;
        metrics["val_reward_v1"]  = best_Rv1;
        metrics["best_thr"]       = best_thr;
        metrics["M_labeled"]      = (int)M;
        metrics["val_size"]       = (int)(M - split);
        metrics["N_rows"]         = (int)N;
        metrics["raw_cols"]       = (int)raw15.n_cols;
        metrics["feat_cols"]      = (int)D;

        metrics["fee_per_trade"]  = fee;
        metrics["alpha_sharpe"]   = a_sh;
        metrics["lambda_risk"]    = lam;
        metrics["mu_manip"]       = mu_m;

        metrics["val_profit_avg"] = profit;
        metrics["val_sharpe"]     = sharpe;
        metrics["val_winrate"]    = winrate;
        metrics["val_drawdown"]   = dd_max;
        metrics["val_reward_v2"]  = reward_v2;
        metrics["val_manip_ratio"]= manip_ratio;

        // MTF контекстная телеметрия
        metrics["wctx_htf"]       = wctx_htf;
        metrics["val_reward_wctx"]= reward_wctx;
        metrics["htf_agree60"]    = htf_agree60;
        metrics["htf_agree240"]   = htf_agree240;

        // Прометеус-гейджи
        etai::set_reward_avg(reward_v2);
        etai::set_reward_sharpe(sharpe);
        etai::set_reward_winrate(winrate);
        etai::set_reward_drawdown(dd_max);
        etai::set_reward_wctx(reward_wctx); // должен существовать (мы его уже вводили)

        json out2;
        out2["ok"]            = true;
        out2["schema"]        = "ppo_pro_v2_reward";
        out2["mode"]          = "pro";
        out2["policy"]        = policy;
        out2["policy_source"] = "learn";
        out2["best_thr"]      = best_thr;
        out2["metrics"]       = metrics;
        out2["version"]       = FEAT_VERSION;

        std::cout << "[TRAIN] PPO_PRO N="<<N<<" D="<<D
                  << " M="<<M<<" val="<<(int)(M - split)
                  << " thr="<<best_thr<<" acc="<<acc
                  << " Sharpe="<<sharpe<<" DD="<<dd_max<<" WinR="<<winrate
                  << " ManipR="<<manip_ratio
                  << " wctx_htf="<<wctx_htf
                  << " feat_ver="<<FEAT_VERSION << std::endl;

        return out2;
    }catch(const std::exception& e){
        out["ok"]=false; out["error"]=e.what();
        return out;
    }
}

} // namespace etai
