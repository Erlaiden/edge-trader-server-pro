#include "ppo_pro.h"
#include "features.h"
#include "json.hpp"
#include <armadillo>
#include <vector>
#include <cmath>
#include <stdexcept>
#include <iostream>

using nlohmann::json;
using namespace arma;

namespace etai {

arma::Mat<double> build_feature_matrix(const arma::Mat<double>&);

// === v8 ===
static constexpr int FEAT_VERSION_CURRENT = 8;

// ---------- utils ----------
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
    const uword D=X.n_cols; W.set_size(D); W.zeros(); b=0.0;
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

// ---------- regime proxy (v8 columns map) ----------
// v8 feature indices:
//  0: ema_spread, 1: rsi, 2: macd, 3: macd_hist, 4: atr, 5: bb_width,
//  6: volr, 7: dPct, 8: corr_v, 9: accel, 10: slope, 11: sr_dist,
// 12: energy, 13: liq, 14: trap_idx, 15: vp_ratio, 16: manip_flag,
// 17: mfi_norm, 18: flow_ratio, 19: cum_flow, 20: sfi,
// 21..27: derived (ratios/tanh combos)
struct Regime {
    int type;        // 0=flat, 1=trend, 2=correction
    double k_reward; // множитель награды
    double tp_mul;
    double sl_mul;
};

static inline Regime classify_row_v8(const rowvec& x){
    const double energy = x(12);
    const double slope  = x(10);
    const double accel  = x(9);
    const double corr   = x(8);

    // Простая и устойчивая логика:
    // flat: низкая энергия и маленький наклон
    if (std::fabs(slope) < 1e-3 && energy < 0.9) {
        return {0, 0.5, 1.0, 1.0};
    }
    // correction: большая перекорреляция к EMA или наклон и коррекция в противофазе
    if (std::fabs(corr) > 1.2 || (slope*accel) < 0.0) {
        return {2, 0.7, 0.6, 0.6};
    }
    // trend (особенно старт импульса)
    if (energy >= 1.05 && slope*accel > 0.0) {
        return {1, 1.3, 1.0, 1.0};
    }
    // по умолчанию — умеренный тренд
    return {1, 1.0, 1.0, 1.0};
}

// ---------- reward v8 (режим, импульс, анти-манип) ----------
static double simulate_reward_v8(const mat& Xva,
                                 const vec& fut_ret,
                                 const vec& proba,
                                 double thr,
                                 double tp,
                                 double sl)
{
    double R=0.0;
    const uword N=fut_ret.n_rows;

    for(uword i=0;i<N;++i){
        double fr = fut_ret(i);
        double pr = proba(i);
        rowvec x = Xva.row(i);

        Regime rg = classify_row_v8(x);

        // анти-манип штраф/гашение
        double manip = 0.0;
        if (Xva.n_cols > 16) manip = x(16); // 0/1

        double tp_use = tp * rg.tp_mul;
        double sl_use = sl * rg.sl_mul;
        double k      = rg.k_reward;

        // штраф за манипуляцию (снижает множитель)
        if (manip > 0.5) {
            k *= 0.6;
            tp_use *= 0.9;
            // sl_use оставим как есть — агрессивный выход из ловушки
        }

        if (pr>=thr){
            if(fr>=tp_use)       R += tp_use * k;
            else if(fr<=-sl_use) R -= sl_use * k;
            else                 R += fr * k;
        }else{
            // вне сделки — «упущенная выгода/экономия убытка»
            if(fr<=-sl_use)      R += tp_use * 0.5;
            else if(fr>=tp_use)  R -= sl_use * 0.5;
            else                 R -= fr * 0.3;
        }
    }
    return R;
}

// ---------- основной тренер ----------
json trainPPO_pro(const arma::mat& raw15,
                  const arma::mat* /*raw60*/,
                  const arma::mat* /*raw240*/,
                  const arma::mat* /*raw1440*/,
                  int /*episodes*/,
                  double tp,
                  double sl,
                  int ma_len,
                  bool /*use_antimanip*/)
{
    json out=json::object();
    try{
        if(raw15.n_cols<6||raw15.n_rows<300){
            out["ok"]=false; out["error"]="bad_raw_shape";
            out["raw_cols"]=(int)raw15.n_cols;
            out["N_rows"]=(int)raw15.n_rows;
            return out;
        }

        mat F=build_feature_matrix(raw15);
        const uword N=F.n_rows, D=F.n_cols;

        // будущий ретёрн на 1 шаг
        vec close=raw15.col(4), r(N,fill::zeros);
        for(uword i=0;i+1<N;++i){ double c0=close(i),c1=close(i+1); r(i)=(c0>0)?(c1/c0-1.0):0.0; }
        vec fut=arma::shift(r,-1); fut(N-1)=0.0;

        // разметка по tp/sl
        double thr_pos=clampd(tp,1e-4,1e-1), thr_neg=clampd(sl,1e-4,1e-1);
        std::vector<uword> idx; idx.reserve(N);
        for(uword i=0;i<N;++i){ double fr=fut(i); if(fr>=thr_pos||fr<=-thr_neg) idx.push_back(i); }
        const uword M=idx.size();
        if(M<200){
            out["ok"]=false; out["error"]="not_enough_labeled";
            out["M_labeled"]=(int)M; out["N_rows"]=(int)N; out["feat_cols"]=(int)D;
            return out;
        }

        // сборка датасета
        mat Xs(M,D,fill::zeros); vec ys(M,fill::zeros), fut_s(M,fill::zeros);
        for(uword k=0;k<M;++k){
            uword i=idx[k];
            Xs.row(k)=F.row(i);
            ys(k)=(fut(i)>=thr_pos)?1.0:0.0;
            fut_s(k)=fut(i);
        }

        // валидация 20%
        uword split=(uword)std::floor(M*0.8);
        if(split==0||split>=M) split = M>1 ? M-1 : 1;

        mat Xtr=Xs.rows(0,split-1); vec ytr=ys.rows(0,split-1);
        mat Xva=Xs.rows(split,M-1);  vec yva=ys.rows(split,M-1);
        vec fr_va=fut_s.rows(split,M-1);

        // нормализация по train
        vec mu=arma::mean(Xtr,0).t(), sd=arma::stddev(Xtr,0,0).t();
        for(uword j=0;j<D;++j){
            double s=(std::isfinite(sd(j))&&sd(j)>1e-12)?sd(j):1.0;
            Xtr.col(j)=(Xtr.col(j)-mu(j))/s;
            Xva.col(j)=(Xva.col(j)-mu(j))/s;
        }

        // логрег
        vec W; double b=0;
        train_logreg(Xtr,ytr,W,b,300,0.05,1e-4);
        vec pv=predict_proba(Xva,W,b);

        // acc@0.5
        vec pred01=conv_to<vec>::from(pv>=0.5);
        double acc=arma::mean(conv_to<vec>::from(pred01==yva));

        // best_thr по вал. награде v8
        double best_thr=0.5, best_R=-1e100;
        for(double thr=0.30; thr<=0.70+1e-9; thr+=0.02){
            double R=simulate_reward_v8(Xva,fr_va,pv,thr,thr_pos,thr_neg);
            if(R>best_R){ best_R=R; best_thr=thr; }
        }
        best_thr=clampd(best_thr,1e-4,0.99);

        // политика и метрики
        json policy;
        policy["W"]=std::vector<double>(W.begin(),W.end());
        policy["b"]={b};
        policy["feat_dim"]=(int)W.n_rows;
        policy["feat_version"]=FEAT_VERSION_CURRENT;
        policy["note"]="logreg_v1";

        json metrics;
        metrics["val_accuracy"]=acc;
        metrics["val_reward"]=best_R;
        metrics["best_thr"]=best_thr;
        metrics["M_labeled"]=(int)M;
        metrics["val_size"]=(int)(M - split);
        metrics["N_rows"]=(int)N;
        metrics["raw_cols"]=6;
        metrics["feat_cols"]=(int)D;

        out["ok"]=true;
        out["schema"]="ppo_pro_v1";
        out["mode"]="pro";
        out["policy"]=policy;
        out["policy_source"]="learn";
        out["best_thr"]=best_thr;
        out["tp"]=tp;
        out["sl"]=sl;
        out["ma_len"]=ma_len;
        out["version"]=FEAT_VERSION_CURRENT;
        out["metrics"]=metrics;

        std::cout<<"[TRAIN] PPO_PRO v8 ok acc="<<acc
                 <<" R="<<best_R
                 <<" thr="<<best_thr
                 <<" D="<<D
                 <<std::endl;

        return out;
    }catch(const std::exception& e){
        out["ok"]=false; out["error"]="ppo_pro_exception"; out["detail"]=e.what(); return out;
    }catch(...){
        out["ok"]=false; out["error"]="ppo_pro_unknown"; return out;
    }
}

} // namespace etai
