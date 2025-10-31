#include "rewardv2_accessors.h"
#include <atomic>
#include <cstdlib>
#include <cmath>

namespace etai {

// --- Телеметрия Reward v2 ---
static std::atomic<double> G_REWARD_AVG{0.0};
static std::atomic<double> G_REWARD_SHARPE{0.0};
static std::atomic<double> G_REWARD_WINRATE{0.0};
static std::atomic<double> G_REWARD_DRAWDOWN{0.0};

// --- Конфиги Reward v2 (ENV) ---
static std::atomic<double> G_FEE_PER_TRADE{0.0005}; // 5 bps
static std::atomic<double> G_ALPHA_SHARPE{0.5};
static std::atomic<double> G_LAMBDA_RISK{1.0};
static std::atomic<double> G_MU_MANIP{0.2};

// --- Anti-manip validation telemetry ---
static std::atomic<double> G_VAL_MANIP_RATIO{0.0};     // нормализованный ratio
static std::atomic<double> G_VAL_MANIP_FLAGGED{0.0};   // count как double

// --- Dynamic coefficients (effective used last) ---
static std::atomic<double> G_LAMBDA_RISK_EFF{1.0};
static std::atomic<double> G_MU_MANIP_EFF{0.2};

// --- Tunables for dynamics ---
static std::atomic<double> G_SIGMA_REF{0.01};    // опорная волатильность
static std::atomic<double> G_LAMBDA_KVOL{2.0};   // чувствительность λ к σ
static std::atomic<double> G_MU_KFREQ{3.0};      // чувствительность μ к частоте флагов

static inline double envd(const char* k, double defv){
    const char* s = std::getenv(k);
    if(!s || !*s) return defv;
    char* e=nullptr;
    double v = std::strtod(s,&e);
    if(e==s || !std::isfinite(v)) return defv;
    return v;
}

// Телеметрия
double get_reward_avg()       { return G_REWARD_AVG.load(); }
double get_reward_sharpe()    { return G_REWARD_SHARPE.load(); }
double get_reward_winrate()   { return G_REWARD_WINRATE.load(); }
double get_reward_drawdown()  { return G_REWARD_DRAWDOWN.load(); }
void   set_reward_avg(double v)      { G_REWARD_AVG.store(std::isfinite(v)?v:0.0); }
void   set_reward_sharpe(double v)   { G_REWARD_SHARPE.store(std::isfinite(v)?v:0.0); }
void   set_reward_winrate(double v)  { G_REWARD_WINRATE.store((v>=0.0&&v<=1.0)?v:0.0); }
void   set_reward_drawdown(double v) { G_REWARD_DRAWDOWN.store(std::isfinite(v)?v:0.0); }

// Конфиги
double get_fee_per_trade() { return G_FEE_PER_TRADE.load(); }
double get_alpha_sharpe()  { return G_ALPHA_SHARPE.load(); }
double get_lambda_risk()   { return G_LAMBDA_RISK.load(); }
double get_mu_manip()      { return G_MU_MANIP.load(); }
void   set_fee_per_trade(double v){ G_FEE_PER_TRADE.store(v>=0.0?v:0.0); }
void   set_alpha_sharpe(double v) { G_ALPHA_SHARPE.store(std::isfinite(v)?v:0.0); }
void   set_lambda_risk(double v)  { G_LAMBDA_RISK.store(std::isfinite(v)?v:0.0); }
void   set_mu_manip(double v)     { G_MU_MANIP.store(std::isfinite(v)?v:0.0); }

void init_rewardv2_from_env(){
    set_fee_per_trade( envd("ETAI_FEE_BPS", 5.0) / 10000.0 );
    set_alpha_sharpe(  envd("ETAI_ALPHA",   0.5) );
    set_lambda_risk(   envd("ETAI_LAMBDA",  1.0) );
    set_mu_manip(      envd("ETAI_MU",      0.2) );
    // dynamics tunables
    G_SIGMA_REF.store(    envd("ETAI_SIGMA_REF",  0.01) );
    G_LAMBDA_KVOL.store(  envd("ETAI_LAMBDA_KVOL",2.0) );
    G_MU_KFREQ.store(     envd("ETAI_MU_KFREQ",   3.0) );
    // reset telemetry
    G_VAL_MANIP_RATIO.store(0.0);
    G_VAL_MANIP_FLAGGED.store(0.0);
    G_LAMBDA_RISK_EFF.store(G_LAMBDA_RISK.load());
    G_MU_MANIP_EFF.store(G_MU_MANIP.load());
}

// Anti-manip telemetry
double get_val_manip_ratio()      { return G_VAL_MANIP_RATIO.load(); }
void   set_val_manip_ratio(double v){
    if(!std::isfinite(v) || v < 0.0) v = 0.0;
    G_VAL_MANIP_RATIO.store(v);
}
double get_val_manip_flagged()    { return G_VAL_MANIP_FLAGGED.load(); }
void   set_val_manip_flagged(double v){
    if(!std::isfinite(v) || v < 0.0) v = 0.0;
    G_VAL_MANIP_FLAGGED.store(v);
}

// Dynamic coeffs
double get_lambda_risk_eff() { return G_LAMBDA_RISK_EFF.load(); }
void   set_lambda_risk_eff(double v){
    if(!std::isfinite(v) || v < 0.0) v = 0.0;
    G_LAMBDA_RISK_EFF.store(v);
}
double get_mu_manip_eff() { return G_MU_MANIP_EFF.load(); }
void   set_mu_manip_eff(double v){
    if(!std::isfinite(v) || v < 0.0) v = 0.0;
    G_MU_MANIP_EFF.store(v);
}

// Tunables getters
double get_sigma_ref()   { return G_SIGMA_REF.load(); }
double get_lambda_kvol() { return G_LAMBDA_KVOL.load(); }
double get_mu_kfreq()    { return G_MU_KFREQ.load(); }

} // namespace etai
