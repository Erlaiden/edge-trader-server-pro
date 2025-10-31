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
    // anti-manip telemetry сбрасываем в 0 на старте
    G_VAL_MANIP_RATIO.store(0.0);
    G_VAL_MANIP_FLAGGED.store(0.0);
}

// Anti-manip telemetry
double get_val_manip_ratio()      { return G_VAL_MANIP_RATIO.load(); }
void   set_val_manip_ratio(double v){
    // допускаем любые >=0 числа; NaN → 0
    if(!std::isfinite(v) || v < 0.0) v = 0.0;
    G_VAL_MANIP_RATIO.store(v);
}
double get_val_manip_flagged()    { return G_VAL_MANIP_FLAGGED.load(); }
void   set_val_manip_flagged(double v){
    if(!std::isfinite(v) || v < 0.0) v = 0.0;
    G_VAL_MANIP_FLAGGED.store(v);
}

} // namespace etai
