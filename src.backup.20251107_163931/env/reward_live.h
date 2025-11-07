#pragma once
#include <cmath>

namespace etai {

// Параметры вознаграждения «PRO»
struct RewardParams {
    // Положительные компоненты
    double w_pf       = 0.60;   // коэффициент к Profit Factor (>=0)
    double w_sharpe   = 0.40;   // коэффициент к Sharpe
    double w_win      = 0.25;   // коэффициент к (winrate-0.5)

    // Штрафы
    double w_dd       = 0.80;   // штраф за max drawdown
    double w_fee      = 1.00;   // прямой штраф за комиссию на трейд * trades

    // Будущее (пока не используем в v1, но оставим интерфейс)
    double w_early    = 0.10;   // бонус за ранний вход (placeholder)
    double w_manip    = 0.20;   // штраф за манипулятивные участки (placeholder)
};

// Нормализации/ограничители
inline double clamp01(double x){ if(std::isnan(x)||!std::isfinite(x)) return 0.0;
    if(x<0.0) return 0.0; if(x>1.0) return 1.0; return x; }

inline double safe_pf(double pf){
    if(!std::isfinite(pf)) return 0.0;
    // сглаженная лог-нормировка: pf 1.0->0, 2.0->~0.69, 3.0->~1.10
    return std::log(std::max(1.0, pf));
}

// Основная формула вознаграждения за эпизод (v1):
// R = + w_pf*log(PF>=1) + w_sharpe*Sharpe + w_win*(winrate-0.5)
//     - w_dd*maxDD - w_fee*(fee_per_trade*trades)
//
// Все величины — агрегаты по эпизоду/валидации.
// Цель — поощрять высокое качество (PF, Sharpe, winrate) и штрафовать риск/комиссии.
inline double reward_live(double profit_factor,
                          double sharpe,
                          double winrate,
                          double max_dd,
                          double fee_per_trade,
                          int    trades,
                          const RewardParams& p = RewardParams())
{
    double pf_term   = p.w_pf     * safe_pf(profit_factor);
    double sh_term   = p.w_sharpe * (std::isfinite(sharpe) ? sharpe : 0.0);
    double win_term  = p.w_win    * (clamp01(winrate) - 0.5);
    double dd_term   = p.w_dd     * (std::isfinite(max_dd) ? max_dd : 0.0);
    double fee_term  = p.w_fee    * std::max(0.0, fee_per_trade) * std::max(0, trades);

    return pf_term + sh_term + win_term - dd_term - fee_term;
}

} // namespace etai
