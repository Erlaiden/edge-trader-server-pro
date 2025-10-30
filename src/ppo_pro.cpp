#include "ppo_pro.h"
#include "features.h"
#include <cmath>
#include <limits>
#include <vector>
#include <iostream>

using arma::mat; using arma::vec; using arma::uvec; using arma::uword;

// Явная декларация нужной функции (как в features.cpp)
namespace etai {
    arma::Mat<double> build_feature_matrix(const arma::Mat<double>& raw);
}

namespace etai {

static inline double clampd(double v, double lo, double hi){
    if(!std::isfinite(v)) return lo;
    if(v<lo) return lo;
    if(v>hi) return hi;
    return v;
}

static vec predict_proba(const mat& X, const vec& W, double b){
    vec z = X * W + b;
    return 1.0 / (1.0 + arma::exp(-z));
}

// простая L2-логрег (GD)
static void train_logreg(const mat& X, const vec& y, vec& W, double& b,
                         int epochs, double lr, double l2){
    const uword D = X.n_cols;
    W = vec(D, arma::fill::zeros);
    b = 0.0;
    for(int e=0; e<epochs; ++e){
        vec p = predict_proba(X, W, b);
        vec gW = X.t() * (p - y) / X.n_rows + l2 * W;
        double gb = arma::mean(p - y);
        W -= lr * gW;
        b -= lr * gb;
        if(!W.is_finite()){ break; }
    }
}

// симуляция вознаграждения на валидации
static double simulate_reward(const vec& fut_ret, const vec& proba,
                              double thr, double tp, double sl){
    const uword N = fut_ret.n_rows;
    double R = 0.0;
    for(uword i=0;i<N;++i){
        double fr = fut_ret(i);
        double p  = proba(i);
        if(p >= thr){
            if(fr >= tp) R += tp;
            else if(fr <= -sl) R -= sl;
            else R += fr;
        }else{
            if(fr <= -sl) R += tp;
            else if(fr >=  tp) R -= sl;
            else R -= fr;
        }
    }
    return R;
}

// future return по close (col=4) со смещением +1
static vec make_future_returns(const mat& raw){
    const uword N = raw.n_rows;
    vec out(N, arma::fill::zeros);
    for(uword i=0;i+1<N;++i){
        double c0 = raw(i,4);
        double c1 = raw(i+1,4);
        if(std::isfinite(c0) && c0>0.0 && std::isfinite(c1)){
            out(i) = c1/c0 - 1.0;
        }else{
            out(i) = std::numeric_limits<double>::quiet_NaN();
        }
    }
    out(N-1) = std::numeric_limits<double>::quiet_NaN();
    return out;
}

nlohmann::json trainPPO_pro(
    const arma::mat& raw15,
    const arma::mat* /*raw60*/,
    const arma::mat* /*raw240*/,
    const arma::mat* /*raw1440*/,
    int /*episodes*/,
    double tp,
    double sl,
    int /*ma_len*/)
{
    nlohmann::json out;
    try{
        const uword R = raw15.n_rows;
        const uword C = raw15.n_cols;

        // 1) фичи и future returns на одном диапазоне
        mat F   = build_feature_matrix(raw15);  // NxD, D=8
        vec fut = make_future_returns(raw15);   // Nx1
        const uword N = F.n_rows;
        const uword D = F.n_cols;

        // 2) маска валидности
        uvec keep(N, arma::fill::zeros);
        for(uword i=0;i<N;++i){
            bool ok = std::isfinite(fut(i));
            if(ok){
                for(uword j=0;j<D;++j){
                    double v = F(i,j);
                    if(!std::isfinite(v)){ ok=false; break; }
                }
            }
            keep(i) = ok ? 1u : 0u;
        }

        uword M0 = arma::accu(keep);
        if(M0==0){
            out["ok"]=false; out["error"]="no_valid_rows_after_mask";
            out["rows"]=R; out["raw_cols"]=C; out["feat_cols"]=D;
            return out;
        }

        mat Xs(M0, D, arma::fill::zeros);
        vec fr_s(M0, arma::fill::zeros);
        for(uword i=0,k=0;i<N;++i){
            if(keep(i)){
                Xs.row(k) = F.row(i);
                fr_s(k)   = fut(i);
                ++k;
            }
        }

        // 3) метки по tp/sl на согласованных рядах
        double thr_pos = clampd(tp, 1e-4, 1e-1);
        double thr_neg = clampd(sl, 1e-4, 1e-1);

        std::vector<uword> idx;
        idx.reserve(M0/5);
        for(uword i=0;i<M0;++i){
            double r = fr_s(i);
            if(r >=  thr_pos || r <= -thr_neg) idx.push_back(i);
        }
        uword M = (uword)idx.size();
        if(M==0){
            out["ok"]=false; out["error"]="not_enough_labeled_samples";
            out["rows"]=R; out["raw_cols"]=C; out["feat_cols"]=D; out["M_labeled"]=0;
            return out;
        }

        mat Xl(M, D, arma::fill::zeros);
        vec yl(M, arma::fill::zeros);
        vec fr_l(M, arma::fill::zeros);
        for(uword k=0;k<M;++k){
            uword i = idx[k];
            Xl.row(k) = Xs.row(i);
            fr_l(k)   = fr_s(i);
            yl(k)     = (fr_s(i) >= thr_pos) ? 1.0 : 0.0;
        }

        // 4) split 80/20
        uword split = (uword)std::floor(M * 0.8);
        if(split==0 || split>=M){
            out["ok"]=false; out["error"]="no_validation_split";
            out["M_labeled"]=M; out["split"]=split;
            return out;
        }

        mat Xtr = Xl.rows(0, split-1);
        vec ytr = yl.rows(0, split-1);
        vec fr_tr = fr_l.rows(0, split-1);

        mat Xva = Xl.rows(split, M-1);
        vec yva = yl.rows(split, M-1);
        vec fr_va = fr_l.rows(split, M-1);

        // 5) нормализация по train
        vec mu = arma::mean(Xtr, 0).t();
        vec sd = arma::stddev(Xtr, 0, 0).t();
        for(uword j=0;j<D;++j){
            double s = (std::isfinite(sd(j)) && sd(j)>1e-12) ? sd(j) : 1.0;
            Xtr.col(j) = (Xtr.col(j) - mu(j)) / s;
            Xva.col(j) = (Xva.col(j) - mu(j)) / s;
        }

        // 6) логрег
        vec W; double b = 0.0;
        train_logreg(Xtr, ytr, W, b, /*epochs*/300, /*lr*/0.05, /*l2*/1e-4);

        // 7) метрики
        vec pv = predict_proba(Xva, W, b);
        if(pv.n_rows==0 || yva.n_rows==0){
            out["ok"]=false; out["error"]="empty_validation_after_logreg";
            out["M_labeled"]=M; out["split"]=split;
            return out;
        }
        vec pred01 = arma::conv_to<vec>::from(pv >= 0.5);
        double acc = arma::mean( arma::conv_to<vec>::from(pred01 == yva) );

        // --- КЛЮЧЕВОЙ ФИКС: поиск лучшего порога вероятности вокруг 0.5 ---
        double best_thr = 0.50;
        double best_R   = -std::numeric_limits<double>::infinity();
        for(double thr = 0.30; thr <= 0.70 + 1e-12; thr += 0.02){
            double Rv = simulate_reward(fr_va, pv, thr, thr_pos, thr_neg);
            if(Rv > best_R){ best_R = Rv; best_thr = thr; }
        }
        best_thr = clampd(best_thr, 1e-3, 0.999);

        // лог
        std::cout << "[TRAIN] PPO_PRO rows=" << R
                  << " raw_cols=" << C
                  << " feat_cols=" << D
                  << " M_labeled=" << M
                  << " split=" << split
                  << " val_size=" << (M - split)
                  << " best_thr=" << best_thr
                  << " best_R=" << best_R
                  << " acc=" << acc
                  << std::endl;

        // ответ
        nlohmann::json policy;
        policy["W"] = std::vector<double>(W.begin(), W.end());
        policy["b"] = { b };
        policy["feat_dim"]     = (int)W.n_rows;
        policy["feat_version"] = 3;
        policy["note"]         = "logreg_v1_masked";

        nlohmann::json metrics;
        metrics["val_accuracy"] = acc;
        metrics["val_reward"]   = best_R;
        metrics["best_thr"]     = best_thr;
        metrics["N_rows"]       = (int)R;
        metrics["raw_cols"]     = (int)C;
        metrics["feat_cols"]    = (int)D;
        metrics["M_labeled"]    = (int)M;
        metrics["val_size"]     = (int)(M - split);

        out["ok"]           = true;
        out["schema"]       = "ppo_pro_v1";
        out["mode"]         = "pro";
        out["policy"]       = policy;
        out["policy_source"]= "learn";
        out["best_thr"]     = best_thr;
        out["metrics"]      = metrics;

        return out;
    }catch(const std::exception& e){
        out["ok"] = false;
        out["error"] = "ppo_pro_exception";
        out["error_detail"] = e.what();
        return out;
    }catch(...){
        out["ok"] = false;
        out["error"] = "ppo_pro_unknown";
        return out;
    }
}

} // namespace etai
