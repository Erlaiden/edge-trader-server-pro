#pragma once
#include <armadillo>
#include <vector>
#include <algorithm>
#include <cmath>

namespace etai {

struct SupportResistanceLevel {
    double price;
    double strength;
    std::string type;
    int touches;
};

struct SRAnalysis {
    double nearest_support;
    double nearest_resistance;
    double distance_to_support;
    double distance_to_resistance;
    std::string position;
    double confidence_boost;
    std::vector<SupportResistanceLevel> levels;
};

inline std::vector<double> find_local_maxima(const arma::mat& M, size_t lookback = 50) {
    std::vector<double> maxima;
    if (M.n_cols < lookback || M.n_rows < 5) return maxima;
    
    size_t N = M.n_cols;
    size_t start = N - lookback;
    
    for (size_t i = start + 2; i < N - 2; ++i) {
        double high = M(2, i);
        bool is_max = true;
        for (int j = -2; j <= 2; ++j) {
            if (j == 0) continue;
            if (M(2, i + j) >= high) {
                is_max = false;
                break;
            }
        }
        if (is_max) maxima.push_back(high);
    }
    return maxima;
}

inline std::vector<double> find_local_minima(const arma::mat& M, size_t lookback = 50) {
    std::vector<double> minima;
    if (M.n_cols < lookback || M.n_rows < 5) return minima;
    
    size_t N = M.n_cols;
    size_t start = N - lookback;
    
    for (size_t i = start + 2; i < N - 2; ++i) {
        double low = M(3, i);
        bool is_min = true;
        for (int j = -2; j <= 2; ++j) {
            if (j == 0) continue;
            if (M(3, i + j) <= low) {
                is_min = false;
                break;
            }
        }
        if (is_min) minima.push_back(low);
    }
    return minima;
}

inline SRAnalysis analyze_support_resistance(const arma::mat& M, size_t lookback = 50) {
    SRAnalysis sr;
    sr.confidence_boost = 0.0;
    sr.nearest_support = 0.0;
    sr.nearest_resistance = 1e9;
    sr.position = "unknown";
    
    if (M.n_cols < lookback || M.n_rows < 5) return sr;
    
    double current_price = M(4, M.n_cols - 1);
    auto maxima = find_local_maxima(M, lookback);
    auto minima = find_local_minima(M, lookback);
    
    for (const auto& price : minima) {
        if (price < current_price && price > sr.nearest_support) {
            sr.nearest_support = price;
        }
    }
    
    for (const auto& price : maxima) {
        if (price > current_price && price < sr.nearest_resistance) {
            sr.nearest_resistance = price;
        }
    }
    
    if (sr.nearest_support > 0) {
        sr.distance_to_support = ((current_price - sr.nearest_support) / current_price) * 100.0;
    } else {
        sr.distance_to_support = 100.0;
    }
    
    if (sr.nearest_resistance < 1e9) {
        sr.distance_to_resistance = ((sr.nearest_resistance - current_price) / current_price) * 100.0;
    } else {
        sr.distance_to_resistance = 100.0;
    }
    
    if (sr.distance_to_support < 1.0) {
        sr.position = "near_support";
        sr.confidence_boost = 15.0;
    } else if (sr.distance_to_resistance < 1.0) {
        sr.position = "near_resistance";
        sr.confidence_boost = -12.0;
    } else if (sr.distance_to_support > 2.0 && sr.distance_to_resistance > 2.0) {
        sr.position = "between";
        sr.confidence_boost = 0.0;
    } else {
        sr.position = "between";
        sr.confidence_boost = 0.0;
    }
    
    return sr;
}

} // namespace etai
