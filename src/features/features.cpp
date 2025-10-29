#include "features.h"
#include <cmath>

namespace etai {

arma::vec compute_ema(const arma::vec& x, int period) {
    arma::vec out(x.n_elem, arma::fill::zeros);
    if (x.n_elem == 0) return out;
    double k = 2.0 / (period + 1.0);
    out(0) = x(0);
    for (size_t i = 1; i < x.n_elem; ++i)
        out(i) = x(i) * k + out(i - 1) * (1.0 - k);
    return out;
}

arma::vec compute_rsi(const arma::vec& close, int period) {
    if (close.n_elem == 0) return arma::vec();
    arma::vec delta = arma::diff(close);
    arma::vec up = arma::clamp(delta, 0, arma::datum::inf);
    arma::vec down = arma::clamp(-delta, 0, arma::datum::inf);
    arma::vec ema_up = compute_ema(up, period);
    arma::vec ema_down = compute_ema(down, period);
    // защита от деления на 0
    ema_down.transform([](double v){ return v < 1e-12 ? 1e-12 : v; });
    arma::vec rs = ema_up / ema_down;
    arma::vec rsi = 100.0 - (100.0 / (1.0 + rs));
    // выравниваем размер до close.n_elem
    rsi.insert_rows(0, 1);
    rsi(0) = rsi(1);
    return rsi;
}

arma::vec compute_momentum(const arma::vec& close, int period) {
    arma::vec mom(close.n_elem, arma::fill::zeros);
    for (size_t i = period; i < close.n_elem; ++i)
        mom(i) = close(i) - close(i - period);
    return mom;
}

arma::mat compute_macd(const arma::vec& close, int fast, int slow, int signal) {
    arma::vec ema_fast = compute_ema(close, fast);
    arma::vec ema_slow = compute_ema(close, slow);
    arma::vec macd = ema_fast - ema_slow;
    arma::vec sig = compute_ema(macd, signal);
    arma::vec hist = macd - sig;
    // 2 x N: [macd; hist]
    arma::mat out = arma::join_cols(macd.t(), hist.t());
    return out;
}

arma::vec compute_atr(const arma::vec& high, const arma::vec& low, const arma::vec& close, int period) {
    arma::vec tr(high.n_elem, arma::fill::zeros);
    for (size_t i = 1; i < high.n_elem; ++i) {
        double hl = high(i) - low(i);
        double hc = std::abs(high(i) - close(i-1));
        double lc = std::abs(low(i) - close(i-1));
        tr(i) = std::max({hl, hc, lc});
    }
    return compute_ema(tr, period);
}

arma::mat compute_bb_width(const arma::vec& close, int period) {
    arma::vec ma(close.n_elem, arma::fill::zeros);
    arma::vec sd(close.n_elem, arma::fill::zeros);
    if (close.n_elem == 0) return arma::mat();
    for (size_t i = period-1; i < close.n_elem; ++i) {
        arma::vec win = close.subvec(i - period + 1, i);
        ma(i) = arma::mean(win);
        sd(i) = arma::stddev(win);
    }
    // защита от нулевого ma
    arma::vec ma_safe = ma;
    ma_safe.transform([](double v){ return std::abs(v) < 1e-12 ? 1e-12 : v; });
    arma::vec width = (sd / ma_safe) * 100.0;
    // 2 x N: [ma; width]
    arma::mat out = arma::join_cols(ma.t(), width.t());
    return out;
}

static inline void vstack_inplace(arma::mat& X, const arma::mat& B) {
    if (X.n_elem == 0) {
        X = B;
    } else if (B.n_elem != 0) {
        X = arma::join_cols(X, B);
    }
}

arma::mat build_feature_matrix(const arma::mat& M) {
    // ожидаемый формат:
    // row0: ts, row1: open, row2: high, row3: low, row4: close, row5: volume
    if (M.n_rows < 5 || M.n_cols == 0) return arma::mat();

    const arma::vec close = M.row(4).t();
    const arma::vec high  = M.row(2).t();
    const arma::vec low   = M.row(3).t();

    arma::vec rsi = compute_rsi(close, 14);
    arma::vec ema_fast = compute_ema(close, 8);
    arma::vec ema_slow = compute_ema(close, 21);
    arma::vec mom = compute_momentum(close, 10);
    arma::vec atr = compute_atr(high, low, close, 14);
    arma::mat macd = compute_macd(close, 12, 26, 9);   // 2xN
    arma::mat bb   = compute_bb_width(close, 20);      // 2xN

    // 1xN представления
    arma::rowvec rsi_r = rsi.t();
    arma::rowvec emas_r = (ema_fast - ema_slow).t();
    arma::rowvec mom_r = mom.t();
    arma::rowvec atr_r = atr.t();

    // поочередная вертикальная склейка
    arma::mat X; // пустая, потом наращиваем
    vstack_inplace(X, rsi_r);
    vstack_inplace(X, emas_r);
    vstack_inplace(X, mom_r);
    vstack_inplace(X, atr_r);
    vstack_inplace(X, macd);
    vstack_inplace(X, bb);

    return X;
}

} // namespace etai
