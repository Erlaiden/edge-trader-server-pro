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

json trainPPO_pro(const arma::mat& raw15,
                  const arma::mat* /*raw60*/,
                  const arma::mat* /*raw240*/,
                  const arma::mat* /*raw1440*/,
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

        // 1) Фичи
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

        // 4) Нормализация
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

        // 8) Anti-manip: считаем raw ratio, затем нормализуем по волатильности
        double manip_ratio_raw = 0.0;
        double manip_ratio_norm = 0.0;
        double manip_vol = 0.0;
        uword manip_flagged = 0, manip_total = 0;

        if(env_enabled("ETAI_ENABLE_ANTI_MANIP")){
            // серии для SR и флагов
            std::vector<double> Vopen(N), Vhigh(N), Vlow(N), Vclose(N);
            for(uword i=0;i<N;++i){ Vopen[i]=open(i); Vhigh[i]=high(i); Vlow[i]=low(i); Vclose[i]=close(i); }

            auto sup = rolling_support(Vlow,  20);
            auto res = rolling_resistance(Vhigh,20);
            auto fbf = false_break_flags(Vopen, Vhigh, Vlow, Vclose, sup, res, /*tol*/5e-4);

            // только валидационный диапазон
            uword a = std::min<uword>(split, (uword)idx.size());
            uword b = (idx.size()==0) ? 0 : (uword)idx.size();
            uword cnt=0, den=0;
            for(uword k=a; k<b; ++k){
                uword i = idx[k];
                if(i < fbf.size()){
                    ++den;
                    if(fbf[i]!=0) ++cnt;
                }
            }
            manip_flagged = cnt;
            manip_total   = den;
            manip_ratio_raw = (den>0)? (double)cnt/(double)den : 0.0;

            // волатильность по доходностям close: sigma(ret)
            if(N > 2){
                std::vector<double> ret; ret.reserve(N-1);
                for(uword i=1;i<N;++i){
                    double c0 = close(i-1), c1 = close(i);
                    double rr = (c0>0.0)? (c1/c0 - 1.0) : 0.0;
                    ret.push_back(rr);
                }
                double m=0.0; for(double v:ret) m+=v; m/= (double)ret.size();
                double var=0.0; for(double v:ret){ double d=v-m; var+=d*d; }
                var/= (double)ret.size();
                manip_vol = std::sqrt(var);
            } else {
                manip_vol = 0.0;
            }

            manip_ratio_norm = manip_ratio_raw / (1.0 + manip_vol);
        }

        // 9) Reward v2 @best_thr: динамические λ и μ
        double fee   = etai::get_fee_per_trade();
        double a_sh  = etai::get_alpha_sharpe();
        double lam_0 = etai::get_lambda_risk();
        double mu_0  = etai::get_mu_manip();

        // динамика
        double sigma_ref = etai::get_sigma_ref();   if(!(sigma_ref>0.0)) sigma_ref = 0.01;
        double k_vol     = etai::get_lambda_kvol();
        double k_freq    = etai::get_mu_kfreq();

        double lam_eff = lam_0 * (1.0 + k_vol  * (manip_vol / (sigma_ref + 1e-12)));
        double mu_eff  = mu_0  * (1.0 + k_freq * (manip_ratio_norm));

        // ограничим разумно
        if(!std::isfinite(lam_eff) || lam_eff<0.0) lam_eff = lam_0;
        if(!std::isfinite(mu_eff)  || mu_eff <0.0) mu_eff  = mu_0;

        vec pnl = pnl_series(fr_va, pv, best_thr, thr_pos, thr_neg, fee);
        double sharpe   = etai::calc_sharpe(pnl, 1e-12, 1.0);
        double dd_max   = etai::calc_max_drawdown(pnl);
        double winrate  = etai::calc_winrate(pnl);
        double profit   = arma::accu(pnl) / std::max<arma::uword>(pnl.n_elem,1);
        double risk     = dd_max;

        double manip_term = env_enabled("ETAI_ENABLE_ANTI_MANIP") ? manip_ratio_norm : manip_ratio_raw;
        double reward_v2  = profit - lam_eff*risk - mu_eff*manip_term + a_sh*sharpe - fee;

        // 10) Политика + метрики
        json policy;
        policy["W"]            = std::vector<double>(W.begin(), W.end());
        policy["b"]            = { b };
        policy["feat_dim"]     = (int)W.n_rows;
        policy["feat_version"] = FEAT_VERSION;
        policy["note"]         = "logreg_v2_reward_dyn";

        json metrics;
        metrics["val_accuracy"]      = acc;
        metrics["val_reward_v1"]     = best_Rv1;
        metrics["best_thr"]          = best_thr;
        metrics["M_labeled"]         = (int)M;
        metrics["val_size"]          = (int)(M - split);
        metrics["N_rows"]            = (int)N;
        metrics["raw_cols"]          = (int)raw15.n_cols;
        metrics["feat_cols"]         = (int)D;

        metrics["fee_per_trade"]     = fee;
        metrics["alpha_sharpe"]      = a_sh;
        metrics["lambda_risk"]       = lam_0;
        metrics["mu_manip"]          = mu_0;
        metrics["val_lambda_eff"]    = lam_eff;
        metrics["val_mu_eff"]        = mu_eff;

        metrics["val_profit_avg"]    = profit;
        metrics["val_sharpe"]        = sharpe;
        metrics["val_winrate"]       = winrate;
        metrics["val_drawdown"]      = dd_max;
        metrics["val_reward_v2"]     = reward_v2;

        // анти-манип метрики
        metrics["val_manip_ratio"]       = manip_ratio_raw;
        metrics["val_manip_ratio_norm"]  = manip_ratio_norm;
        metrics["val_manip_flagged"]     = (int)manip_flagged;
        metrics["val_manip_vol"]         = manip_vol;

        etai::set_reward_avg(reward_v2);
        etai::set_reward_sharpe(sharpe);
        etai::set_reward_winrate(winrate);
        etai::set_reward_drawdown(dd_max);
        etai::set_val_manip_ratio(manip_ratio_norm);
        etai::set_val_manip_flagged((double)manip_flagged);
        etai::set_lambda_risk_eff(lam_eff);
        etai::set_mu_manip_eff(mu_eff);

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
                  << " ManipR_raw="<<manip_ratio_raw
                  << " ManipR_norm="<<manip_ratio_norm
                  << " lam_eff="<<lam_eff<<" mu_eff="<<mu_eff
                  << " feat_ver="<<FEAT_VERSION << std::endl;

        return out2;
    }catch(const std::exception& e){
        out["ok"]=false; out["error"]=e.what();
        return out;
    }
}

} // namespace etai
