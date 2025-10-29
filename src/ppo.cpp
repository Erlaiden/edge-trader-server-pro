#include "ppo.h"
#include <armadillo>
#include <cmath>
#include <algorithm>
#include <random>
#include <ctime>
#include <vector>
#include <utility>

using json = nlohmann::json;

namespace etai {

// === helpers ===
static arma::vec rolling_mean(const arma::vec& x, int w) {
  arma::vec out(x.n_elem, arma::fill::zeros);
  double acc = 0.0;
  for (size_t i = 0; i < x.n_elem; i++) {
    acc += x(i);
    if ((int)i >= w) acc -= x(i - w);
    if ((int)i >= w - 1) out(i) = acc / w;
  }
  return out;
}

static arma::vec price_to_returns(const arma::vec& close) {
  arma::vec ret = arma::zeros(close.n_elem);
  for (size_t i = 1; i < close.n_elem; i++)
    ret(i) = (close(i) - close(i - 1)) / close(i - 1);
  return ret;
}

// === ВНУТРЕННЯЯ оценочная функция (используется в PRO-поиске) ===
json evalPPO_internal(const arma::mat& M,
                      int episodes,
                      double tp_pct,
                      double sl_pct,
                      int ma_len) {
  if (M.n_rows < 5 || M.n_cols < (size_t)(ma_len + 2)) {
    return json{{"ok", false}, {"error", "not_enough_data"}};
  }

  const arma::vec open  = M.row(1).t();
  const arma::vec high  = M.row(2).t();
  const arma::vec low   = M.row(3).t();
  const arma::vec close = M.row(4).t();

  arma::vec ret = price_to_returns(close);
  arma::vec rma = rolling_mean(ret, ma_len);

  auto evaluate = [&](double thr) {
    double totalReward = 0.0;
    int trades = 0, wins = 0;
    for (size_t i = ma_len + 1; i + 1 < close.n_elem; i++) {
      double s = rma(i);
      int action = 0;
      if (std::abs(s) > thr) action = (s > 0) ? +1 : -1;

      if (action != 0) {
        double entry    = close(i);
        double tp_price = entry * (1.0 + (action > 0 ? tp_pct : -tp_pct));
        double sl_price = entry * (1.0 - (action > 0 ? sl_pct : -sl_pct));
        double r = 0.0;

        // conservative intrabar: SL приоритетнее TP
        if ((action > 0 && low(i + 1)  <= sl_price) ||
            (action < 0 && high(i + 1) >= sl_price)) {
          r = -sl_pct;
        } else if ((action > 0 && high(i + 1) >= tp_price) ||
                   (action < 0 && low(i + 1)  <= tp_price)) {
          r = tp_pct;
        } else {
          r = (close(i + 1) - entry) / entry;
          if (action < 0) r = -r;
        }

        totalReward += r;
        trades++; if (r > 0) wins++;
      }
    }
    double acc = (trades > 0) ? (double)wins / trades : 0.0;
    return std::pair<double,double>(totalReward, acc);
  };

  // random search for threshold
  std::mt19937_64 rng(42);
  std::normal_distribution<double> noise(0.0, 0.0005);
  double thr = 0.0005;
  auto best = evaluate(thr);
  double best_thr = thr, best_reward = best.first, best_acc = best.second;

  for (int ep = 0; ep < episodes; ++ep) {
    double cand = std::max(1e-5, thr + noise(rng));
    auto rr = evaluate(cand);
    if (rr.first >= best_reward) {
      thr = cand; best_reward = rr.first; best_acc = rr.second; best_thr = cand;
    } else {
      thr = 0.9*thr + 0.1*best_thr;
    }
  }

  // agents
  json agents = json::array();
  auto eval_agent = [&](const char* name, int sign) {
    double tot=0.0; int trades=0, wins=0;
    std::vector<double> eq; eq.reserve(close.n_elem);
    double equity=0.0, peak=0.0, dd=0.0;

    for (size_t i = ma_len + 1; i + 1 < close.n_elem; i++) {
      double s = rma(i) * sign;
      int act = (s > best_thr) ? +1 : 0;
      if (act != 0) {
        double entry = close(i);
        double tp_price = entry * (1.0 + (sign > 0 ? tp_pct : -tp_pct));
        double sl_price = entry * (1.0 - (sign > 0 ? sl_pct : -sl_pct));
        double r = 0.0;

        if ((sign > 0 && low(i + 1)  <= sl_price) ||
            (sign < 0 && high(i + 1) >= sl_price)) {
          r = -sl_pct;
        } else if ((sign > 0 && high(i + 1) >= tp_price) ||
                   (sign < 0 && low(i + 1)  <= tp_price)) {
          r = tp_pct;
        } else {
          r = (close(i + 1) - entry) / entry;
          if (sign < 0) r = -r;
        }

        equity += r;
        peak = std::max(peak, equity);
        dd = std::max(dd, peak - equity);
        eq.push_back(equity);
        tot += r; trades++; if (r > 0) wins++;
      } else {
        eq.push_back(equity);
        peak = std::max(peak, equity);
        dd = std::max(dd, peak - equity);
      }
    }

    double acc = (trades ? (double)wins / trades : 0.0);
    double mean=0.0, sd=0.0;
    if (!eq.empty()) {
      mean = eq.back() / (double)eq.size();
      double v=0.0; for (double x: eq) v += (x-mean)*(x-mean);
      sd = (eq.size()>1)? std::sqrt(v/(eq.size()-1)) : 0.0;
    }
    double sharpe = (sd>1e-12)? (mean/sd) : 0.0;

    return json{
      {"name", name},
      {"thr",  sign>0? best_thr : -best_thr},
      {"totalReward", tot},
      {"trades", trades},
      {"winrate", acc},
      {"accuracy", acc},
      {"sharpe", sharpe},
      {"drawdown", dd},
      {"expectancy", trades? tot/trades:0.0}
    };
  };

  agents.push_back(eval_agent("LONG", +1));
  agents.push_back(eval_agent("SHORT", -1));

  // neutral (volatility)
  double sigma_thr = 0.001;
  int calm_bars = 0;
  int total_bars = (int)close.n_elem;
  arma::vec ret2 = price_to_returns(close);
  for (size_t i = ma_len + 20; i < ret2.n_elem; i++) {
    double sigma = arma::stddev(ret2.rows(i - 20, i - 1));
    if (sigma < sigma_thr) calm_bars++;
  }
  double flat_rate = total_bars ? (double)calm_bars / (double)total_bars : 0.0;

  agents.push_back(json{
    {"name","NEUTRAL"},
    {"vol_threshold", sigma_thr},
    {"calm_bars", calm_bars},
    {"flat_rate", flat_rate},
    {"comment","Periods of low volatility; signal=flat"}
  });

  return json{
    {"ok", true},
    {"build_ts", (long long)time(nullptr)*1000},
    {"episodes", episodes},
    {"tp", tp_pct},
    {"sl", sl_pct},
    {"ma_len", ma_len},
    {"best_thr", best_thr},
    {"totalReward", best_reward},
    {"accuracy", best_acc},
    {"intrabar", "ohlc_check_conservative"},
    {"agents", agents}
  };
}

// === single-TF inference (kept) ===
json infer_with_threshold(const arma::mat& M, double best_thr, int ma_len) {
  if (M.n_rows < 5 || M.n_cols < (size_t)(ma_len + 1)) {
    return json{{"ok", false}, {"error", "not_enough_data"}};
  }
  const arma::vec close = M.row(4).t();
  arma::vec ret = price_to_returns(close);
  arma::vec rma = rolling_mean(ret, ma_len);

  double s = rma(rma.n_elem - 1);
  double sigma = arma::stddev(ret.tail(20));
  double sigma_thr = 0.001;
  std::string signal = "NEUTRAL";

  if (sigma >= sigma_thr) {
    if (std::abs(s) > best_thr) signal = (s > 0) ? "LONG" : "SHORT";
  }

  return json{
    {"ok", true},
    {"score", s},
    {"signal", signal},
    {"sigma", sigma},
    {"vol_threshold", sigma_thr}
  };
}

// === multi-timeframe inference ===
json infer_mtf(const arma::mat& M15, double thr15, int ma15,
               const arma::mat* M60,   int ma60,
               const arma::mat* M240,  int ma240,
               const arma::mat* M1440, int ma1440) {
  // 15m
  if (M15.n_rows < 5 || M15.n_cols < (size_t)(ma15 + 1)) {
    return json{{"ok", false}, {"error", "not_enough_data_15"}};
  }
  const arma::vec close15 = M15.row(4).t();
  arma::vec ret15 = price_to_returns(close15);
  arma::vec rma15 = rolling_mean(ret15, ma15);
  double s15 = rma15(rma15.n_elem - 1);

  // волатильность на 15
  double sigma15 = arma::stddev(ret15.tail(20));
  double vol_thr = 0.001;
  std::string signal = "NEUTRAL";
  if (sigma15 >= vol_thr) {
    if (std::abs(s15) > thr15) signal = (s15 > 0) ? "LONG" : "SHORT";
  }

  // Старшие ТФ
  auto compute_s = [&](const arma::mat* M, int ma) -> std::pair<bool,double> {
    if (!M) return {false, 0.0};
    if (M->n_rows < 5 || M->n_cols < (size_t)(ma + 1)) return {false, 0.0};
    arma::vec c = M->row(4).t();
    arma::vec r = price_to_returns(c);
    arma::vec rm = rolling_mean(r, ma);
    return {true, rm(rm.n_elem - 1)};
  };

  auto eps_for = [&](int htf_minutes) {
    double scale = std::sqrt(15.0 / (double)htf_minutes);
    double eps = std::max(1e-5, thr15 * scale);
    return eps;
  };

  json htf = json::object();

  if (M60) {
    auto [ok60, s60] = compute_s(M60, ma60);
    if (ok60) {
      double eps60 = eps_for(60);
      bool strong = std::abs(s60) > eps60;
      bool agree  = (s15 == 0.0) ? false : ((s60 > 0) == (s15 > 0));
      htf["60"] = {{"score", s60}, {"eps", eps60}, {"strong", strong}, {"agree", agree}};
      if (signal != "NEUTRAL" && strong && !agree) signal = "NEUTRAL";
    }
  }
  if (M240) {
    auto [ok240, s240] = compute_s(M240, ma240);
    if (ok240) {
      double eps240 = eps_for(240);
      bool strong = std::abs(s240) > eps240;
      bool agree  = (s15 == 0.0) ? false : ((s240 > 0) == (s15 > 0));
      htf["240"] = {{"score", s240}, {"eps", eps240}, {"strong", strong}, {"agree", agree}};
      if (signal != "NEUTRAL" && strong && !agree) signal = "NEUTRAL";
    }
  }
  if (M1440) {
    auto [ok1440, s1440] = compute_s(M1440, ma1440);
    if (ok1440) {
      double eps1440 = eps_for(1440);
      bool strong = std::abs(s1440) > eps1440;
      bool agree  = (s15 == 0.0) ? false : ((s1440 > 0) == (s15 > 0));
      htf["1440"] = {{"score", s1440}, {"eps", eps1440}, {"strong", strong}, {"agree", agree}};
      if (signal != "NEUTRAL" && strong && !agree) signal = "NEUTRAL";
    }
  }

  return json{
    {"ok", true},
    {"signal", signal},
    {"score15", s15},
    {"sigma15", sigma15},
    {"vol_threshold", vol_thr},
    {"htf", htf}
  };
}

// === batch inference with HTF filters ===
json infer_mtf_batch(const arma::mat& M15, double thr15, int ma15,
                     const arma::mat* M60,   int ma60,
                     const arma::mat* M240,  int ma240,
                     const arma::mat* M1440, int ma1440,
                     int last_n) {
  if (M15.n_rows < 5 || M15.n_cols < (size_t)(ma15 + 21)) {
    return json{{"ok", false}, {"error", "not_enough_data_15"}};
  }
  const arma::rowvec ts15 = M15.row(0);
  const arma::vec close15 = M15.row(4).t();
  arma::vec ret15 = price_to_returns(close15);
  arma::vec rma15 = rolling_mean(ret15, ma15);

  auto build_scores = [&](const arma::mat* M, int ma) {
    std::vector<std::pair<long long,double>> out;
    if (!M) return out;
    if (M->n_rows < 5 || M->n_cols < (size_t)(ma + 1)) return out;
    const arma::rowvec ts = M->row(0);
    arma::vec c = M->row(4).t();
    arma::vec r = price_to_returns(c);
    arma::vec rm = rolling_mean(r, ma);
    size_t start = std::max( (size_t)(ma+1), (size_t)1 );
    out.reserve(M->n_cols);
    for (size_t i = start; i < M->n_cols; ++i) {
      out.emplace_back( (long long)ts(i), rm(i) );
    }
    return out;
  };

  std::vector<std::pair<long long,double>> s60  = build_scores(M60,  ma60);
  std::vector<std::pair<long long,double>> s240 = build_scores(M240, ma240);
  std::vector<std::pair<long long,double>> s1440= build_scores(M1440,ma1440);

  auto eps_for = [&](int htf_minutes) {
    double scale = std::sqrt(15.0 / (double)htf_minutes);
    return std::max(1e-5, thr15 * scale);
  };
  double eps60 = eps_for(60);
  double eps240 = eps_for(240);
  double eps1440 = eps_for(1440);
  double vol_thr = 0.001;

  size_t i60=0, i240=0, i1440=0;

  size_t end = M15.n_cols; // not inclusive
  size_t start = (end > (size_t)last_n) ? end - (size_t)last_n : 0;
  size_t warmup = std::max((size_t)(ma15 + 20), (size_t)1);
  if (start < warmup) start = warmup;

  json items = json::array();

  for (size_t i = start; i < end; ++i) {
    long long ts = (long long)ts15(i);
    double s15 = rma15(i);
    double sigma15 = arma::stddev(ret15.rows(i - 20, i - 1));

    std::string sig = "NEUTRAL";
    if (sigma15 >= vol_thr) {
      if (std::abs(s15) > thr15) sig = (s15 > 0) ? "LONG" : "SHORT";
    }

    auto step_idx = [&](std::vector<std::pair<long long,double>>& v, size_t& idx){
      while (idx + 1 < v.size() && v[idx+1].first <= ts) idx++;
      return (idx < v.size() && v[idx].first <= ts) ? std::optional<double>(v[idx].second) : std::optional<double>();
    };

    auto v60  = step_idx(s60, i60);
    auto v240 = step_idx(s240, i240);
    auto v1440= step_idx(s1440, i1440);

    // Построим htf-объект для текущего бара
    json htf = json::object();
    auto put_htf = [&](const char* key, const std::optional<double>& v, double eps) {
      if (!v.has_value()) return;
      bool strong = std::abs(*v) > eps;
      bool agree  = (s15 == 0.0) ? false : ((*v > 0) == (s15 > 0));
      htf[key] = {{"score", *v}, {"eps", eps}, {"strong", strong}, {"agree", agree}};
      if (sig != "NEUTRAL" && strong && !agree) sig = "NEUTRAL";
    };
    if (v60)   put_htf("60",   v60,   eps60);
    if (v240)  put_htf("240",  v240,  eps240);
    if (v1440) put_htf("1440", v1440, eps1440);

    items.push_back(json{
      {"ts", ts},
      {"signal", sig},
      {"score15", s15},
      {"sigma15", sigma15},
      {"htf", htf}
    });
  }

  return json{
    {"ok", true},
    {"interval", "15"},
    {"n", (int)items.size()},
    {"vol_threshold", vol_thr},
    {"thr15", thr15},
    {"ma15", ma15},
    {"items", items}
  };
}

} // namespace etai
