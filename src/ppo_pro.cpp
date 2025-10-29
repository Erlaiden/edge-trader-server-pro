// === ppo_pro.cpp (fixed: separate Adam for W and b) ===
#include "ppo_pro.h"
#include "ppo.h"
#include "utils.h"
#include "optim/adam.h"
#include "features/features.h"
#include <armadillo>
#include <fstream>
#include <random>
#include <ctime>
#include <filesystem>
#include <cmath>
#include <algorithm>
#include <vector>
#include <cstdlib>
#include <cstring>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace etai {

// ===== ENV =====
static inline int get_env_int(const char* name, int defv) {
    if (!name) return defv;
    const char* s = std::getenv(name);
    if (!s || !*s) return defv;
    char* end = nullptr;
    long v = std::strtol(s, &end, 10);
    if (end == s) return defv;
    return (int)v;
}
static inline unsigned long long get_env_u64(const char* name, unsigned long long defv) {
    if (!name) return defv;
    const char* s = std::getenv(name);
    if (!s || !*s) return defv;
    char* end = nullptr;
    unsigned long long v = std::strtoull(s, &end, 10);
    if (end == s) return defv;
    return v;
}
static inline std::string get_env_str(const char* name, const char* defv){
    const char* s = std::getenv(name);
    return (s && *s) ? std::string(s) : std::string(defv ? defv : "");
}

// ===== helpers =====
static arma::vec rolling_mean(const arma::vec& x, int w) {
    arma::vec out(x.n_elem, arma::fill::zeros);
    double acc = 0.0;
    for (size_t i = 0; i < x.n_elem; i++) {
        acc += x(i);
        if ((int)i >= w) acc -= x(i - w);
        if ((int)i >= w - 1) out(i) = acc / w;
    }
    return out;
}
static arma::vec price_to_returns(const arma::vec& close) {
    arma::vec ret = arma::zeros(close.n_elem);
    for (size_t i = 1; i < close.n_elem; i++)
        ret(i) = (close(i) - close(i - 1)) / close(i - 1);
    return ret;
}
static inline double clamp(double v, double a, double b){ return std::max(a, std::min(v, b)); }

// ===== Simple Policy/Value (PRO learn) =====
// FIX: отдельные Adam для W и для b, чтобы формы совпадали
struct PolicyNet {
    arma::mat W;  // 1 x D
    arma::vec b;  // 1 x 1 (vector of size 1)
    Adam optW;
    Adam optB;

    PolicyNet(int in_dim, double lr=0.001) : W(1, in_dim), b(1), optW(lr), optB(lr) {
        W = arma::randn(1, in_dim) * 0.01;
        b.zeros();
    }
    double forward_scalar(const arma::vec& x) const {
        double z = arma::as_scalar(W * x + b(0));
        return std::tanh(z); // [-1,1]
    }
    void update(const arma::vec& x, double adv, double scale=1.0) {
        arma::mat gradW = adv * x.t();          // 1 x D
        arma::vec gradB(1); gradB(0) = adv;     // 1

        W = optW.step(W, gradW * scale);
        b = optB.step(b, gradB * scale);
    }
};

struct ValueNet {
    arma::mat W;  // 1 x D
    arma::vec b;  // 1
    Adam optW;
    Adam optB;

    ValueNet(int in_dim, double lr=0.001) : W(1, in_dim), b(1), optW(lr), optB(lr) {
        W = arma::randn(1, in_dim) * 0.01;
        b.zeros();
    }
    double forward_scalar(const arma::vec& x) const {
        return arma::as_scalar(W * x + b(0)); // R
    }
    void update(const arma::vec& x, double target, double scale=1.0) {
        double v = forward_scalar(x);
        double err = v - target;                    // d/dW (1/2 (v-target)^2) = err * x
        arma::mat gradW = err * x.t();             // 1 x D
        arma::vec gradB(1); gradB(0) = err;        // 1

        W = optW.step(W, gradW * scale);
        b = optB.step(b, gradB * scale);
    }
};

// ===== Metrics for CV backtest =====
struct EvalRes {
    double reward=0.0, accuracy=0.0, expectancy=0.0, sharpe=0.0, drawdown=0.0;
    int    trades=0;
    double sharpe_long=0.0, sharpe_short=0.0;
    int    trades_long=0, trades_short=0;
};

static EvalRes eval_on_range(
    const arma::mat& M, double tp, double sl, int ma, double thr,
    size_t start_idx, size_t end_idx)
{
    EvalRes R{};
    const arma::vec close = M.row(4).t();
    const arma::vec low   = M.row(3).t();
    const arma::vec high  = M.row(2).t();

    arma::vec ret = price_to_returns(close);
    arma::vec rma = rolling_mean(ret, ma);

    auto sim_side = [&](int sign){
        double tot=0.0, equity=0.0, peak=0.0, dd=0.0; int trades=0, wins=0;
        std::vector<double> eq; eq.reserve(end_idx);
        size_t s = std::max(start_idx, (size_t)(ma+1));
        for (size_t i=s; i+1<end_idx; ++i) {
            double sig = rma(i)*sign;
            if (sig > thr) {
                double entry = close(i);
                double tp_p = entry*(1.0+(sign>0?tp:-tp));
                double sl_p = entry*(1.0-(sign>0?sl:-sl));
                double r=0.0;
                if ((sign>0 && low(i+1)<=sl_p) || (sign<0 && high(i+1)>=sl_p)) r=-sl;
                else if ((sign>0 && high(i+1)>=tp_p) || (sign<0 && low(i+1)<=tp_p)) r=tp;
                else { r=(close(i+1)-entry)/entry; if (sign<0) r=-r; }
                equity+=r; peak=std::max(peak,equity); dd=std::max(dd,peak-equity);
                eq.push_back(equity); tot+=r; trades++; if (r>0) wins++;
            } else {
                eq.push_back(equity); peak=std::max(peak,equity); dd=std::max(dd,peak-equity);
            }
        }
        double acc= trades? (double)wins/trades : 0.0;
        double mean=0.0, sd=0.0;
        if (!eq.empty()) {
            mean = eq.back()/(double)eq.size();
            double v=0; for (double x: eq) v+=(x-mean)*(x-mean);
            sd=(eq.size()>1)?sqrt(v/(eq.size()-1)):0;
        }
        double sh=(sd>1e-12)?(mean/sd):0.0;
        EvalRes E{};
        E.reward=tot; E.trades=trades; E.accuracy=acc; E.drawdown=dd; E.sharpe=sh;
        return E;
    };

    EvalRes L=sim_side(+1), S=sim_side(-1);
    R.reward=L.reward+S.reward;
    R.trades=L.trades+S.trades;
    R.accuracy=(R.trades?((L.accuracy*L.trades+S.accuracy*S.trades)/R.trades):0.0);
    R.expectancy=(R.trades?R.reward/R.trades:0.0);
    R.drawdown=std::max(L.drawdown,S.drawdown);
    R.sharpe_long=L.sharpe; R.sharpe_short=S.sharpe;
    R.sharpe=0.5*(L.sharpe+S.sharpe);
    R.trades_long=L.trades; R.trades_short=S.trades;
    return R;
}

struct FoldRes { EvalRes isr, oosr; size_t is_start,is_end,oos_start,oos_end; };

static std::vector<FoldRes> walk_forward_cv(const arma::mat& M,int folds,double tp,double sl,int ma,double thr){
    std::vector<FoldRes> out;
    if (M.n_rows<5||M.n_cols<(size_t)(ma+22)||folds<=1) return out;
    size_t N=M.n_cols, warmup=(size_t)(ma+20);
    size_t usable=(N>(warmup+1))?(N-(warmup+1)):0;
    if (usable<(size_t)folds) return out;
    size_t oos_len=std::max((size_t)std::floor((double)usable/folds),(size_t)50);
    for(int k=0;k<folds;++k){
        size_t is_end=warmup+k*oos_len; if(is_end<=warmup) continue;
        size_t oos_start=is_end;
        size_t oos_end=std::min(oos_start+oos_len,N-1);
        EvalRes IS=eval_on_range(M,tp,sl,ma,thr,warmup,is_end);
        EvalRes OOS=eval_on_range(M,tp,sl,ma,thr,oos_start,oos_end);
        out.push_back({IS,OOS,warmup,is_end,oos_start,oos_end});
        if(oos_end>=N-1) break;
    }
    return out;
}

static json summarize_cv(const std::vector<FoldRes>& CV,bool oos){
    auto get=[&](const FoldRes& f)->const EvalRes&{return oos?f.oosr:f.isr;};
    double r=0,a=0,e=0,d=0,s=0,sL=0,sS=0,t=0,tL=0,tS=0;int n=0;
    for(auto&f:CV){const EvalRes&R=get(f);r+=R.reward;a+=R.accuracy;e+=R.expectancy;d=std::max(d,R.drawdown);s+=R.sharpe;sL+=R.sharpe_long;sS+=R.sharpe_short;t+=R.trades;tL+=R.trades_long;tS+=R.trades_short;n++;}
    if(!n)n=1;
    return json{{"folds",n},{"reward",r/n},{"accuracy",a/n},{"expectancy",e/n},{"drawdown_max",d},{"sharpe",s/n},{"sharpe_long",sL/n},{"sharpe_short",sS/n},{"trades",t/n},{"trades_long",tL/n},{"trades_short",tS/n}};
}

// ===== normalize features per-row =====
static arma::mat zscore_rows(const arma::mat& X) {
    arma::mat Z = X;
    for (size_t r=0; r<Z.n_rows; ++r) {
        arma::rowvec row = Z.row(r);
        double mean = arma::mean(row);
        double sd = arma::stddev(row);
        if (sd < 1e-12) sd = 1.0;
        Z.row(r) = (row - mean) / sd;
    }
    return Z;
}

// ===== reward from next bar with TP/SL =====
static inline double nextbar_pnl(const arma::vec& close, const arma::vec& high, const arma::vec& low,
                                 size_t i, int sign, double tp, double sl) {
    double entry = close(i);
    double tp_p = entry*(1.0+(sign>0?tp:-tp));
    double sl_p = entry*(1.0-(sign>0?sl:-sl));
    if ((sign>0 && low(i+1)<=sl_p) || (sign<0 && high(i+1)>=sl_p)) return -sl;
    if ((sign>0 && high(i+1)>=tp_p) || (sign<0 && low(i+1)<=tp_p)) return tp;
    double r=(close(i+1)-entry)/entry;
    return (sign>0)? r : -r;
}

// ===== PRO training core: two modes =====
nlohmann::json trainPPO_pro(const arma::mat& M15,const arma::mat*,const arma::mat*,const arma::mat*,
                            int episodes,double tp_init,double sl_init,int ma_init){
    if(M15.n_rows<5||M15.n_cols<50) return json{{"ok",false},{"error","not_enough_data"}};

    const std::string MODE = get_env_str("ETAI_PRO_MODE","search"); // "search" | "learn"
    const long long ts_now=(long long)time(nullptr)*1000;
    unsigned long long seed = get_env_u64("ETAI_SEED", 42ULL);
    int FOLDS = get_env_int("ETAI_CV_FOLDS", 5);
    if (FOLDS < 3) FOLDS = 3;
    if (FOLDS > 10) FOLDS = 10;

    // ========= MODE: search (legacy grid) =========
    if (MODE != "learn") {
        std::mt19937_64 rng(seed);
        std::uniform_real_distribution<double>d_thr(0.0003,0.0012);
        std::uniform_real_distribution<double>d_tp(std::max(0.001,tp_init*0.7),std::max(0.002,tp_init*1.3));
        std::uniform_real_distribution<double>d_sl(std::max(0.001,sl_init*0.7),std::max(0.002,sl_init*1.3));
        std::uniform_int_distribution<int>d_ma(std::max(8,ma_init-4),ma_init+8);

        struct Cand{double thr,tp,sl;int ma;double score_oos,score_reward,score_dd;json cv,is_sum,oos_sum;};
        std::vector<Cand>pool;pool.reserve(episodes);

        const size_t warmup=(size_t)(std::max(1,ma_init)+20);
        long long ts_min=(M15.n_cols? (long long)M15.row(0)(0):0);
        long long ts_max=(M15.n_cols? (long long)M15.row(0)(M15.n_cols-1):0);

        for(int ep=0;ep<episodes;++ep){
            double thr=d_thr(rng),tp=d_tp(rng),sl=d_sl(rng);int ma=d_ma(rng);
            auto CV=walk_forward_cv(M15,FOLDS,tp,sl,ma,thr);
            if(CV.empty()) continue;
            json is_sum=summarize_cv(CV,false),oos_sum=summarize_cv(CV,true);
            double oos_sh=oos_sum.value("sharpe",0.0);
            double oos_rw=oos_sum.value("reward",0.0);
            double oos_dd=oos_sum.value("drawdown_max",0.0);
            json jcv=json::array();
            for(auto&f:CV)
                jcv.push_back({
                    {"is",  {{"reward",f.isr.reward},{"accuracy",f.isr.accuracy},{"expectancy",f.isr.expectancy},{"sharpe",f.isr.sharpe},{"drawdown",f.isr.drawdown}}},
                    {"oos", {{"reward",f.oosr.reward},{"accuracy",f.oosr.accuracy},{"expectancy",f.oosr.expectancy},{"sharpe",f.oosr.sharpe},{"drawdown",f.oosr.drawdown}}},
                    {"ranges",{{"is_start",(long long)f.is_start},{"oos_start",(long long)f.oos_start},{"oos_end",(long long)f.oos_end}}}
                });
            pool.push_back({thr,tp,sl,ma,oos_sh,oos_rw,oos_dd,jcv,is_sum,oos_sum});
        }
        if(pool.empty()) return json{{"ok",false},{"error","no_candidates"}};

        std::sort(pool.begin(),pool.end(),[](const auto&a,const auto&b){
            if(a.score_oos!=b.score_oos) return a.score_oos>b.score_oos;
            if(a.score_reward!=b.score_reward) return a.score_reward>b.score_reward;
            return a.score_dd<b.score_dd;
        });
        const auto best=pool.front();

        fs::create_directories("cache/logs");
        char buf[64];std::time_t tt=ts_now/1000;std::strftime(buf,sizeof(buf),"%Y%m%d_%H%M%S",std::localtime(&tt));
        std::string log_path=std::string("cache/logs/pro_cv_15_")+buf+".json";
        {std::ofstream f(log_path);if(f)f<<json{{"top",{{"thr",best.thr},{"tp",best.tp},{"sl",best.sl},{"ma",best.ma},{"oos_sharpe",best.score_oos},{"folds",FOLDS},{"seed",(unsigned long long)seed}}}}.dump(2);}

        size_t train_used=(M15.n_cols>warmup)?(M15.n_cols-warmup):0;
        int cv_effective = (int)best.oos_sum.value("folds", (int)best.cv.size());

        return json{
            {"ok",true},{"version",3},{"schema","ppo_pro_v1"},{"build_ts",ts_now},
            {"tp",best.tp},{"sl",best.sl},{"ma_len",best.ma},{"best_thr",best.thr},
            {"train_rows_total",(int)M15.n_cols},{"warmup_bars",(int)warmup},{"train_rows_used",(int)train_used},
            {"data_time_range",{{"ts_min",ts_min},{"ts_max",ts_max}}},
            {"cv_folds",FOLDS},{"cv_effective_folds", cv_effective},
            {"cv",best.cv},{"is_summary",best.is_sum},{"oos_summary",best.oos_sum},
            {"log_path",log_path}
        };
    }

    // ========= MODE: learn =========
    const arma::vec ts    = M15.row(0).t();
    const arma::vec open  = M15.row(1).t();
    const arma::vec high  = M15.row(2).t();
    const arma::vec low   = M15.row(3).t();
    const arma::vec close = M15.row(4).t();

    arma::mat X = build_feature_matrix(M15); // D x N
    if (X.n_elem == 0) return json{{"ok",false},{"error","features_empty"}};
    X = zscore_rows(X);

    const size_t N = X.n_cols;
    if (N < 200) return json{{"ok",false},{"error","not_enough_feature_rows"}};

    const size_t oos_tail = 100;
    const size_t train_end = (N > oos_tail+1) ? (N - oos_tail - 1) : (N-2);
    const size_t warmup = 32;
    if (train_end <= warmup+1) return json{{"ok",false},{"error","not_enough_train_window"}};

    const int D = (int)X.n_rows;
    double lr = 0.001;
    PolicyNet policy(D, lr);
    ValueNet  vnet(D, lr);

    int epochs = 20;
    double tp = std::max(0.001, tp_init);
    double sl = std::max(0.001, sl_init);
    double act_gate = 0.10;
    double adv_scale = 1.0;

    for (int ep=0; ep<epochs; ++ep) {
        double ep_reward = 0.0;
        for (size_t i = warmup; i < train_end; ++i) {
            arma::vec x = X.col(i);
            double a = policy.forward_scalar(x); // [-1,1]
            if (std::abs(a) < act_gate) continue;

            int sign = (a >= 0.0) ? +1 : -1;
            double r = nextbar_pnl(close, high, low, i, sign, tp, sl); // [-sl, tp]
            r = clamp(r, -sl, tp);

            double v = vnet.forward_scalar(x);
            double adv = r - v;

            policy.update(x, adv, adv_scale);
            vnet.update(x, r,  adv_scale);

            ep_reward += r;
        }
        (void)ep_reward;
    }

    // OOS на последних 100 барах
    double tot=0.0, equity=0.0, peak=0.0, dd=0.0;
    int trades=0, wins=0, tL=0, tS=0;
    std::vector<double> eq; eq.reserve(oos_tail);
    for (size_t i = train_end; i+1 < N; ++i) {
        arma::vec x = X.col(i);
        double a = policy.forward_scalar(x);
        if (std::abs(a) < act_gate) { eq.push_back(equity); peak=std::max(peak,equity); dd=std::max(dd,peak-equity); continue; }
        int sign = (a >= 0.0) ? +1 : -1;
        double r = nextbar_pnl(close, high, low, i, sign, tp, sl);
        tot += r; trades++; if (r>0) wins++;
        if (sign>0) tL++; else tS++;
        equity += r; peak = std::max(peak, equity); dd = std::max(dd, peak-equity);
        eq.push_back(equity);
    }
    double acc = trades ? (double)wins/trades : 0.0;
    double mean=0.0, sd=0.0;
    if (!eq.empty()) {
        mean = eq.back()/(double)eq.size();
        double v=0; for (double x: eq) v+=(x-mean)*(x-mean);
        sd=(eq.size()>1)?sqrt(v/(eq.size()-1)):0;
    }
    double sh = (sd>1e-12)?(mean/sd):0.0;
    double ex = trades? tot/(double)trades : 0.0;

    fs::create_directories("cache/logs");
    char buf[64];std::time_t tt=ts_now/1000;std::strftime(buf,sizeof(buf),"%Y%m%d_%H%M%S",std::localtime(&tt));
    std::string log_path=std::string("cache/logs/pro_learn_15_")+buf+".json";
    {
        json j{
            {"top", {{"epochs",epochs},{"lr",lr},{"tp",tp},{"sl",sl},{"act_gate",act_gate},
                     {"oos", {{"reward",tot},{"trades",trades},{"accuracy",acc},{"sharpe",sh},{"drawdown_max",dd}}}}},
            {"policy", {
                {"feat_dim", D},
                {"W", std::vector<double>(policy.W.begin(), policy.W.end())},
                {"b", std::vector<double>(policy.b.begin(), policy.b.end())}
            }}
        };
        std::ofstream f(log_path); if (f) f << j.dump(2);
    }

    long long ts_min=(M15.n_cols? (long long)M15.row(0)(0):0);
    long long ts_max=(M15.n_cols? (long long)M15.row(0)(M15.n_cols-1):0);
    size_t train_used = train_end - warmup;

    json is_sum{{"folds", 1},{"reward", 0.0},{"accuracy", 0.0},{"expectancy", 0.0},{"drawdown_max", 0.0},{"sharpe", 0.0},{"sharpe_long", 0.0},{"sharpe_short", 0.0},{"trades", 0.0},{"trades_long", 0.0},{"trades_short", 0.0}};
    json oos_sum{{"folds", 1},{"reward", tot},{"accuracy", acc},{"expectancy", ex},{"drawdown_max", dd},{"sharpe", sh},{"sharpe_long", 0.0},{"sharpe_short", 0.0},{"trades", (double)trades},{"trades_long", (double)tL},{"trades_short", (double)tS}};

    json out{
        {"ok", true},
        {"version", 3},
        {"schema", "ppo_pro_v1"},
        {"build_ts", ts_now},
        {"tp", tp},
        {"sl", sl},
        {"ma_len", ma_init},
        {"best_thr", 0.0006}, // compatibility with legacy infer
        {"train_rows_total", (int)M15.n_cols},
        {"warmup_bars", (int)warmup},
        {"train_rows_used", (int)train_used},
        {"data_time_range", {{"ts_min", ts_min}, {"ts_max", ts_max}}},
        {"cv_folds", 1},
        {"cv_effective_folds", 1},
        {"cv", json::array()},
        {"is_summary", is_sum},
        {"oos_summary", oos_sum},
        {"log_path", log_path},
        {"policy", {
            {"feat_version", 1},
            {"feat_dim", D},
            {"W", std::vector<double>(policy.W.begin(), policy.W.end())},
            {"b", std::vector<double>(policy.b.begin(), policy.b.end())}
        }}
    };
    return out;
}

} // namespace etai
