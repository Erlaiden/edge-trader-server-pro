#pragma once
#include <armadillo>

// Построение набора признаков (RSI, EMA, MACD, ATR, BB-width, Momentum)
namespace etai {
    arma::mat build_feature_matrix(const arma::mat& ohlcv);

    arma::vec compute_rsi(const arma::vec& close, int period=14);
    arma::vec compute_ema(const arma::vec& x, int period);
    arma::vec compute_atr(const arma::vec& high, const arma::vec& low, const arma::vec& close, int period=14);
    arma::mat compute_macd(const arma::vec& close, int fast=12, int slow=26, int signal=9);
    arma::mat compute_bb_width(const arma::vec& close, int period=20);
    arma::vec compute_momentum(const arma::vec& close, int period=10);
}
