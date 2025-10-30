#include "ppo_pro.h"
#include "features.h"      // реализация build_feature_matrix в etai::
#include "json.hpp"
#include <armadillo>
#include <vector>
#include <cmath>
#include <stdexcept>
#include <iostream>

using nlohmann::json;
using namespace arma;

// ---- Мостик имён ------------------------------------------------------------
// В проекте исторически зовём buildfeature_matrix (глобальная, без '_').
// Реальная функция — etai::build_feature_matrix (с '_').
// Делаем обёртку, чтобы не трогать features.* и остальные места.
namespace etai { arma::Mat<double> build_feature_matrix(const arma::Mat<double>&); }

static inline arma::Mat<double> buildfeature_matrix(const arma::Mat<double>& raw){
    return etai::build_feature_matrix(raw);
}
// -----------------------------------------------------------------------------

namespace etai {

// --- helpers ---
static inline double clampd(double v, double lo, double hi){
    if(!std::isfinite(v)) return lo;
    if(v < lo) return lo;
    if(v > hi) return hi;
    return v;
}

static inline double sigmoid(double z){
    if(!std::isfinite(z)) z = 0.0;
    return 1.0 / (1.0 + std::exp(-z));
}

// простая логрег на градиентном спуске
static void train_logreg(const mat& X, const vec& y, vec& W, double& b, int epochs, double lr, double l2){
    const uword D = X.n_cols;
    W.set_size(D);
    W.zeros();
    b = 0.0;

    for(int e=0; e<epochs; ++e){
        vec z = X*W + b;             // N
        vec p = 1.0 / (1.0 + arma::exp(-z));
        vec g = X.t() * (p - y) / X.n_rows + l2 * W;   // D
        double gb = arma::accu(p - y) / X.n_rows;

        W -= lr * g;
        b -= lr * gb;
    }
}

static vec predict_proba(const mat& X, const vec& W, double b){
    vec z = X*W + b;
    for(uword i=0;i<z.n_rows;++i) z(i) = sigmoid(z(i));
    return z;
}

// суммарная «награда» на валидации при пороге proba thr и tp/sl
static double simulate_reward(const vec& fut_ret, const vec& proba, double thr, double tp, double sl){
    double R = 0.0;
    const uword N = fut_ret.n_rows;
    for(uword i=0;i<N;++i){
        double fr = fut_ret(i);
        double pr = proba(i);
        if(pr >= thr){
            if(fr >= tp)       R += tp;
            else if(fr <= -sl) R -= sl;
            else               R += fr;
        }else{
            if(fr <= -sl)      R += tp;
            else if(fr >= tp)  R -= sl;
            else               R -= fr;
        }
    }
    return R;
}

nlohmann::json trainPPO_pro(const arma::mat& raw15,
                            const arma::mat* /*raw60*/,
                            const arma::mat* /*raw240*/,
                            const arma::mat* /*raw1440*/,
                            int /*episodes*/,
                            double tp,
                            double sl,
                            int /*ma_len*/)
{
    json out = json::object();
    try{
        if(raw15.n_cols < 6 || raw15.n_rows < 300){
            out["ok"]=false;
            out["error"]="bad_raw_shape";
            out["raw_cols"]= (int)raw15.n_cols;
            out["N_rows"]  = (int)raw15.n_rows;
std::cout << "[AUDIT_PPO] out=" << out.dump() << std::endl;
            return out;
        }

        // билдим фичи через мостик (глобальная сигнатура)
        mat F = buildfeature_matrix(raw15); // N×D (ожидаем D=8)
        const uword N = F.n_rows;
        const uword D = F.n_cols;

        // future return (1 шаг)
        vec close = raw15.col(4);
        vec r = vec(N, fill::zeros);
        for(uword i=0;i+1<N;++i){
            double c0 = close(i), c1 = close(i+1);
            r(i) = (c0>0.0) ? (c1/c0 - 1.0) : 0.0;
        }
        vec fut = arma::shift(r, -1); fut(N-1)=0.0;

        // разметка
        double thr_pos = clampd(tp, 1e-4, 1e-1);
        double thr_neg = clampd(sl, 1e-4, 1e-1);
        std::vector<uword> idx; idx.reserve(N);
        for(uword i=0;i<N;++i){
            double fr = fut(i);
            if(fr >= thr_pos || fr <= -thr_neg) idx.push_back(i);
        }
        const uword M = (uword)idx.size();
        if(M < 200){
            out["ok"]=false;
            out["error"]="not_enough_labeled_samples";
            out["M_labeled"] = (int)M;
            out["N_rows"]    = (int)N;
            out["raw_cols"]  = (int)raw15.n_cols;
            out["feat_cols"] = (int)D;
std::cout << "[AUDIT_PPO] out=" << out.dump() << std::endl;
            return out;
        }

        mat Xs(M, D, fill::zeros);
        vec ys(M,    fill::zeros);
        vec fut_s(M, fill::zeros);
        for(uword k=0;k<M;++k){
            uword i = idx[k];
            Xs.row(k) = F.row(i);
            ys(k)     = (fut(i) >= thr_pos) ? 1.0 : 0.0;
            fut_s(k)  = fut(i);
        }

        uword split = (uword)std::floor(M * 0.8);
        if(split==0 || split>=M) split = M>1 ? M-1 : 1;

        mat Xtr = Xs.rows(0, split-1);
        vec ytr = ys.rows(0, split-1);
        mat Xva = Xs.rows(split, M-1);
        vec yva = ys.rows(split, M-1);
        vec fr_va = fut_s.rows(split, M-1);

        // нормализация по train
        vec mu = arma::mean(Xtr, 0).t();
        vec sd = arma::stddev(Xtr, 0, 0).t();
        for(uword j=0;j<D;++j){
            double s = (std::isfinite(sd(j)) && sd(j)>1e-12) ? sd(j) : 1.0;
            Xtr.col(j) = (Xtr.col(j) - mu(j)) / s;
            Xva.col(j) = (Xva.col(j) - mu(j)) / s;
        }

        // логрег
        vec W; double b = 0.0;
        train_logreg(Xtr, ytr, W, b, /*epochs*/300, /*lr*/0.05, /*l2*/1e-4);
        vec pv = predict_proba(Xva, W, b);

        // acc@0.5
        vec pred01 = conv_to<vec>::from(pv >= 0.5);
        double acc = arma::mean( conv_to<vec>::from(pred01 == yva) );

        // поиск best_thr по proba в [0.30..0.70]
        double best_thr = 0.50, best_R = -1e100;
        for(double thr=0.30; thr<=0.70+1e-9; thr+=0.02){
            double R = simulate_reward(fr_va, pv, thr, thr_pos, thr_neg);
            if(R > best_R){ best_R = R; best_thr = thr; }
        }
        best_thr = clampd(best_thr, 1e-4, 0.99);

        json policy;
        policy["W"]            = std::vector<double>(W.begin(), W.end());
        policy["b"]            = { b };
        policy["feat_dim"]     = (int)W.n_rows;
        policy["feat_version"] = 3;
        policy["note"]         = "logreg_v1";

        json metrics;
        metrics["val_accuracy"] = acc;
        metrics["val_reward"]   = best_R;
        metrics["best_thr"]     = best_thr;
        metrics["M_labeled"]    = (int)M;
        metrics["val_size"]     = (int)(M - split);
        metrics["N_rows"]       = (int)N;
        metrics["raw_cols"]     = (int)raw15.n_cols;
        metrics["feat_cols"]    = (int)D;

        out["ok"]            = true;
        out["schema"]        = "ppo_pro_v1";
        out["mode"]          = "pro";
        out["policy"]        = policy;
        out["policy_source"] = "learn";
        out["best_thr"]      = best_thr;
        out["metrics"]       = metrics;

        std::cout << "[TRAIN] PPO_PRO rows="<<N
                  << " raw_cols="<<raw15.n_cols
                  << " feat_cols="<<D
                  << " M_labeled="<<M
                  << " split="<<(int)split
                  << " val_size="<<(int)(M-split)
                  << " best_thr="<<best_thr
                  << " best_R="<<best_R
                  << " acc="<<acc
                  << std::endl;

std::cout << "[AUDIT_PPO] out=" << out.dump() << std::endl;
        return out;
    }catch(const std::exception& e){
std::cout << "[AUDIT_PPO] out=" << out.dump() << std::endl;
        out["ok"]=false; out["error"]="ppo_pro_exception"; out["error_detail"]=e.what(); return out;
    }catch(...){
std::cout << "[AUDIT_PPO] out=" << out.dump() << std::endl;
        out["ok"]=false; out["error"]="ppo_pro_unknown"; return out;
    }
}

} // namespace etai
