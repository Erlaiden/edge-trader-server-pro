#pragma once
#include <vector>
namespace etai {
// Роллинговые уровни поддержки/сопротивления по Low/High
std::vector<double> rolling_support(const std::vector<double>& low, int win = 20);
std::vector<double> rolling_resistance(const std::vector<double>& high, int win = 20);
}
