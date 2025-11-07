#pragma once
#include "json.hpp"
#include <vector>
#include <armadillo>

namespace etai {

struct FlowMetrics {
    std::vector<double> mfi;         // [0..100]
    std::vector<double> flow_ratio;  // относительный поток
    std::vector<double> cum_flow;    // кумулятивный поток
    std::vector<double> sfi;         // SmartFlowIndex [~0..]
};

FlowMetrics compute_money_flow(const std::vector<double>& open,
                               const std::vector<double>& high,
                               const std::vector<double>& low,
                               const std::vector<double>& close,
                               const std::vector<double>& volume);

nlohmann::json money_flow_to_json(const std::vector<double>& open,
                                  const std::vector<double>& high,
                                  const std::vector<double>& low,
                                  const std::vector<double>& close,
                                  const std::vector<double>& volume);

} // namespace etai
