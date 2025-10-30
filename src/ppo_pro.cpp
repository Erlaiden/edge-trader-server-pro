#include "json.hpp"
#include <armadillo>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include <stdexcept>

using json = nlohmann::json;
using arma::mat;
using arma::vec;
using arma::uword;

// --- утилиты ---
static inline double clampd(double v, double lo, double hi){
    if(!std::isfinite(v)) return lo;
    if(v<lo) return lo;
    if(v>hi) return hi;
    return v;
}

// скользящее среднее по вектору r с окном w (простая реализация)
static vec rolling_mean(const vec& r, int w){
    const uword n = r.n_rows;
    vec out(n, arma::fill::zeros);
    if(w<=1) { out = r; return out; }
    double s = 0.0;
    for(uword i=0;i<n;i++){
        s += r(i);
        if(i+1 >= (uword)w){
            if(i+1 > (uword)w) s -= r(i-w+1);
            out(i) = s / (double)w;
        } else {
            out(i) = s / double(i+1);
        }
    }
    return out;
}

static vec rolling_std(const vec& r, int w){
    const uword n = r.n_rows;
    vec out(n, arma::fill::zeros);
    if(w<=1){
        out.zeros();
        return out;
    }
    // простая, но устойчивая оценка: std последних w значений
    for(uword i=0;i<n;i++){
        uword a = (i+1 >= (uword)w) ? (i-w+1) : 0;
        uword b = i;
        uword m = b - a + 1;
        double mean = 0.0;
        for(uword k=a;k<=b;k++) mean += r(k);
        mean /= double(m);
        double s2 = 0.0;
        for(uword k=a;k<=b;k++){
            double d = r(k) - mean;
            s2 += d*d;
        }
        s2 = (m>1) ? (s2 / double(m-1)) : 0.0;
        out(i) = std::sqrt(std::max(0.0, s2));
    }
    return out;
}

// безопасный логистический спуск; возвращает (W, b)
static void train_logreg(const mat& X, const vec& y, vec& W, double& b,
                         int epochs=200, double lr=0.05, double l2=1e-4)
{
    const uword N = X.n_rows;
    const uword D = X.n_cols;
    W.set_size(D);
    W.zeros();
    b = 0.0;

    for(int ep=0; ep<epochs; ++ep){
        // p = sigmoid(X*W + b)
        vec z = X*W + b;
        vec p = 1.0 / (1.0 + arma::exp(-z)); // (0,1)
        // градиенты
        vec err = (p - y); // N
        vec gW = (X.t() * err) / double(N) + l2 * W;
        double gb = arma::mean(err);

        // шаг
        W -= lr * gW;
        b -= lr * gb;
    }
}

// предсказание вероятностей (sigmoid)
static vec predict_proba(const mat& X, const vec& W, double b){
    vec z = X*W + b;
    return 1.0 / (1.0 + arma::exp(-z));
}

// симуляция “totalReward” на валидации по будущему возврату (tp/sl)
static double simulate_reward(const vec& fut_ret, const vec& proba, double thr, double tp, double sl){
    // действие: long если proba >= 0.5+thr, short если proba <= 0.5-thr, иначе 0
    double R = 0.0;
    const double up_thr = 0.5 + thr;
    const double dn_thr = 0.5 - thr;
    const uword N = fut_ret.n_rows;
    for(uword i=0;i<N;i++){
        double p = proba(i);
        double fr = fut_ret(i);
        if(p >= up_thr){
            // long: если future >= tp → +tp; если <= -sl → -sl; иначе fr
            if(fr >= tp) R += tp; else if(fr <= -sl) R -= sl; else R += fr;
        } else if(p <= dn_thr){
            // short: если future <= -sl → +tp (симметрично), если >= tp → -sl; иначе -fr
            if(fr <= -sl) R += tp; else if(fr >= tp) R -= sl; else R -= fr;
        } else {
            // no trade
        }
    }
    return R;
}

// --- Публичное API ---
// raw15: N x 6 (ts, open, high, low, close, volume), как в pipeline
namespace etai {

json trainPPO_pro(const arma::mat& raw15,
                  const arma::mat* /*raw60*/,
                  const arma::mat* /*raw240*/,
                  const arma::mat* /*raw1440*/,
                  int episodes, double tp, double sl, int ma_len)
{
    json out = json::object();
    try{
        if(raw15.n_cols < 6 || raw15.n_rows < 300){
            out["ok"] = false;
            out["error"] = "insufficient_data";
            return out;
        }
        // Извлекаем базовые ряды
        const uword N = raw15.n_rows;
        vec close = raw15.col(4);           // close
        vec vol   = raw15.col(5);           // volume

        // Доходности r_t = (C_t/C_{t-1} - 1)
        vec r = arma::zeros<vec>(N);
        for(uword i=1;i<N;i++){
            double c0 = close(i-1);
            double c1 = close(i);
            double rr = (c0>0.0) ? (c1/c0 - 1.0) : 0.0;
            if(!std::isfinite(rr)) rr = 0.0;
            r(i) = rr;
        }

        // Будущий возврат (один шаг вперёд)
        vec fut = arma::zeros<vec>(N);
        for(uword i=0;i+1<N;i++){
            double c0 = close(i);
            double c1 = close(i+1);
            double fr = (c0>0.0) ? (c1/c0 - 1.0) : 0.0;
            if(!std::isfinite(fr)) fr = 0.0;
            fut(i) = fr;
        }

        // Нормализация объёма
        vec vchg = arma::zeros<vec>(N);
        for(uword i=1;i<N;i++){
            double v0 = vol(i-1);
            double v1 = vol(i);
            double d = (v0>0.0) ? (v1/v0 - 1.0) : 0.0;
            if(!std::isfinite(d)) d = 0.0;
            vchg(i) = d;
        }

        // Базовые скользящие характеристики
        int w_ma = std::max(3, std::min(ma_len, 64));
        vec r_ma  = rolling_mean(r,  w_ma);
        vec r_std = rolling_std (r,  w_ma);

        // Ещё немного лагов для информативности
        vec r_lag1 = arma::shift(r,  1);
        vec r_lag2 = arma::shift(r,  2);
        vec v_lag1 = arma::shift(vchg,1);

        // Фичи (D=8), как ты и хотел ранее
        // X cols: [r, r_lag1, r_lag2, r_ma, r_std, vchg, v_lag1, 1]
        const uword D = 8;
        mat X(N, D, arma::fill::zeros);
        for(uword i=0;i<N;i++){
            X(i,0) = r(i);
            X(i,1) = r_lag1(i);
            X(i,2) = r_lag2(i);
            X(i,3) = r_ma(i);
            X(i,4) = r_std(i);
            X(i,5) = vchg(i);
            X(i,6) = v_lag1(i);
            X(i,7) = 1.0; // bias helper (оставим, но b тоже храним отдельно)
        }

        // Целевые метки для обучения (0/1) — берём только уверенные случаи по tp/sl
        std::vector<uword> idx;
        idx.reserve(N);
        double thr_pos = clampd(tp, 1e-4, 1e-1);
        double thr_neg = clampd(sl, 1e-4, 1e-1);

        for(uword i=0;i+1<N;i++){
            if(fut(i) >=  thr_pos || fut(i) <= -thr_neg) idx.push_back(i);
        }
        if(idx.size() < 200){
            out["ok"]   = false;
            out["error"]= "not_enough_labeled_samples";
            out["count"]= (unsigned)idx.size();
            return out;
        }

        // Сформируем обучающую выборку/валидацию (80/20, по времени)
        uword M = (uword)idx.size();
        uword split = (uword)std::floor(M * 0.8);

        mat Xs(M, D, arma::fill::zeros);
        vec ys(M, arma::fill::zeros);
        vec fut_s(M, arma::fill::zeros);

        for(uword k=0;k<M;k++){
            uword i = idx[k];
            for(uword j=0;j<D;j++) Xs(k,j) = X(i,j);
            ys(k) = (fut(i) >= thr_pos) ? 1.0 : 0.0; // 1=long, 0=short
            fut_s(k) = fut(i);
        }

        mat Xtr = Xs.rows(0, split-1);
        vec ytr = ys.rows(0, split-1);
        vec fr_tr = fut_s.rows(0, split-1);

        mat Xva = Xs.rows(split, M-1);
        vec yva = ys.rows(split, M-1);
        vec fr_va = fut_s.rows(split, M-1);

        // Нормализация фичей (по train), чтобы не разлеталось
        vec mu = arma::mean(Xtr, 0).t();          // D x 1
        vec sd = arma::stddev(Xtr, 0, 0).t();     // D x 1 (норм. по N-1)
        for(uword j=0;j<D;j++){
            double s = (std::isfinite(sd(j)) && sd(j)>1e-12) ? sd(j) : 1.0;
            Xtr.col(j) = (Xtr.col(j) - mu(j)) / s;
            Xva.col(j) = (Xva.col(j) - mu(j)) / s;
        }

        // Обучаем логистическую регрессию
        vec W;
        double b = 0.0;
        train_logreg(Xtr, ytr, W, b, /*epochs*/300, /*lr*/0.05, /*l2*/1e-4);

        // Оценка на валидации
        vec pv = predict_proba(Xva, W, b); // [0,1]
        // точность при 0.5
        vec pred01 = arma::conv_to<vec>::from(pv >= 0.5);          // 1/0
        double acc = arma::mean( arma::conv_to<vec>::from(pred01 == yva) );

        // Поиск best_thr на сетке [0.0002..0.01]
        double best_thr = 0.0006;
        double best_R = -1e100;
        for(double thr = 2e-4; thr <= 1e-2 + 1e-9; thr += 2e-4){
            double R = simulate_reward(fr_va, pv, thr, thr_pos, thr_neg);
            if(R > best_R){ best_R = R; best_thr = thr; }
        }
        best_thr = clampd(best_thr, 1e-4, 1e-2);

        // Соберём ответ
        json policy;
        policy["W"] = std::vector<double>(W.begin(), W.end());
        policy["b"] = { b };
        policy["feat_dim"]     = (int)W.n_rows;
        policy["feat_version"] = 2;
        policy["note"]         = "logreg_v1";

        json metrics;
        metrics["val_accuracy"] = acc;
        metrics["val_reward"]   = best_R;
        metrics["best_thr"]     = best_thr;

        out["ok"]          = true;
        out["schema"]      = "ppo_pro_v1";
        out["mode"]        = "pro";
        out["policy"]      = policy;
        out["policy_source"]= "learn";
        out["best_thr"]    = best_thr;
        out["metrics"]     = metrics;

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
