#include "env/env_trading.h"
#include <algorithm>
#include <cmath>

namespace etai {

void EnvTrading::set_dataset(const std::vector<std::vector<double>>& feats,
                             const std::vector<double>& closes,
                             const EnvConfig& cfg)
{
  feats_  = feats;
  closes_ = closes;
  cfg_    = cfg;
  if (!feats_.empty() && !feats_[0].empty())
    cfg_.feat_dim = static_cast<int>(feats_[0].size());
  t_ = 0;
  equity_ = cfg_.start_equity;
  peak_ = equity_;
  max_dd_ = 0.0;
}

std::vector<double> EnvTrading::reset(std::uint64_t){
  t_ = 0;
  equity_ = cfg_.start_equity;
  peak_ = equity_;
  max_dd_ = 0.0;
  if (feats_.empty()) return {};
  return feats_[0];
}

StepIO EnvTrading::step(int action){
  StepIO o{};
  if (feats_.empty() || closes_.size() < 2) { o.done = true; return o; }
  if (t_ + 1 >= feats_.size() || t_ + 1 >= closes_.size()) {
    o.done = true;
    o.next_state = feats_.back();
    return o;
  }

  // Доходность по close: r_t = c_{t+1}/c_t - 1
  double c0 = closes_[t_];
  double c1 = closes_[t_+1];
  double ret = (c0 > 0.0) ? (c1/c0 - 1.0) : 0.0;

  // Нормированный reward: action * ret - fee*|action|
  double fee = std::abs(action) * cfg_.fee_per_trade;
  double rw  = static_cast<double>(action) * ret - fee;

  equity_ += rw;
  peak_ = std::max(peak_, equity_);
  if (peak_ > 0.0) max_dd_ = std::max(max_dd_, (peak_ - equity_) / peak_);

  ++t_;
  o.next_state = feats_[t_];
  o.reward = rw;
  o.done = (t_ + 1 >= feats_.size() || t_ + 1 >= closes_.size());
  return o;
}

} // namespace etai
