#pragma once
#include <armadillo>

namespace etai {

// Простая реализация Adam для векторов и матриц Armadillo
struct Adam {
    double lr;      // learning rate
    double beta1;
    double beta2;
    double eps;
    arma::mat m;
    arma::mat v;
    int t = 0;

    Adam(double lr_=0.001, double b1=0.9, double b2=0.999, double eps_=1e-8)
        : lr(lr_), beta1(b1), beta2(b2), eps(eps_) {}

    void init(const arma::mat& shape_like) {
        m.zeros(shape_like.n_rows, shape_like.n_cols);
        v.zeros(shape_like.n_rows, shape_like.n_cols);
        t = 0;
    }

    arma::mat step(const arma::mat& w, const arma::mat& grad) {
        if (m.n_elem == 0) init(grad);
        t++;
        m = beta1 * m + (1 - beta1) * grad;
        v = beta2 * v + (1 - beta2) * arma::square(grad);
        arma::mat m_hat = m / (1 - std::pow(beta1, t));
        arma::mat v_hat = v / (1 - std::pow(beta2, t));
        arma::mat w_new = w - lr * m_hat / (arma::sqrt(v_hat) + eps);
        return w_new;
    }
};

} // namespace etai
