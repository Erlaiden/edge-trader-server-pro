#pragma once
#include <vector>

namespace etai {

// Money Flow Index (MFI), период по умолчанию 14
std::vector<double> calc_mfi(const std::vector<double>& high,
                             const std::vector<double>& low,
                             const std::vector<double>& close,
                             const std::vector<double>& volume,
                             int period = 14);

// Потоковый коэффициент ~ [0..1] на основе MFI (упрощённо: MFI/100)
std::vector<double> calc_flow_ratio(const std::vector<double>& mfi);

// Кумулятивный поток (центрирован и нормирован к [-1..1] условно)
std::vector<double> calc_cum_flow(const std::vector<double>& flow_ratio);

// SFI — Smart Flow Index: взвешенный поток ([-1..1])
std::vector<double> calc_sfi(const std::vector<double>& flow_ratio,
                             const std::vector<double>& mfi);

} // namespace etai
