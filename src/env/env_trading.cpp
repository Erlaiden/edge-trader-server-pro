#include "env/env_trading.h"
#include <algorithm>

namespace etai {

void EnvTrading::set_dataset(const std::vector<std::vector<double>>& feats){
  feats_ = feats;
  if (!feats_.empty()) {
    if (cfg_.feat_dim <= 0) cfg_.feat_dim = static_cast<int>(feats_[0].size());
  }
  t_ = 0;
  equity_ = 1.0;
  peak_equity_ = 1.0;
  max_dd_ = 0.0;
}

std::vector<double> EnvTrading::reset(std::uint64_t){
  t_ = 0;
  equity_ = 1.0;
  peak_equity_ = 1.0;
  max_dd_ = 0.0;
  if (feats_.empty()) return {};
  return feats_[0];
}

StepIO EnvTrading::step(int /*action*/){
  StepIO o{};
  if (feats_.empty()) { o.done = true; return o; }
  if (t_ + 1 >= feats_.size()) { o.done = true; o.next_state = feats_.back(); return o; }

  // Заглушка без экономической логики: reward=0
  ++t_;
  o.next_state = feats_[t_];
  o.reward = 0.0;
  o.done = (t_ + 1 >= feats_.size());

  // Обновление dd в нуле
  peak_equity_ = std::max(peak_equity_, equity_);
  if (peak_equity_ > 0.0) max_dd_ = std::max(max_dd_, (peak_equity_ - equity_) / peak_equity_);
  return o;
}

} // namespace etai
