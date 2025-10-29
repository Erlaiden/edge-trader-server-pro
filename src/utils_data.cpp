#include "utils_data.h"
#include "fetch.h"      // etai::load_cached_matrix
#include <armadillo>
#include <algorithm>

using json = nlohmann::json;

int minutes_of(const std::string& interval) {
  if (interval == "15")   return 15;
  if (interval == "60")   return 60;
  if (interval == "240")  return 240;
  if (interval == "1440") return 1440;
  return 15;
}

json data_health_report(const std::string& symbol, const std::string& interval) {
  arma::mat M = etai::load_cached_matrix(symbol, interval);
  json out{
    {"interval", interval},
    {"symbol",   symbol},
    {"ok",       M.n_elem > 0},
    {"rows",     (int)M.n_cols}
  };
  if (M.n_elem == 0) return out;

  const arma::rowvec ts = M.row(0);
  int gaps = 0, dups = 0;
  long long ts_min = (long long)ts(0);
  long long ts_max = (long long)ts(M.n_cols - 1);

  long long step_ms = (long long)minutes_of(interval) * 60LL * 1000LL;
  for (size_t i = 1; i < M.n_cols; ++i) {
    long long dt = (long long)ts(i) - (long long)ts(i - 1);
    if (dt > step_ms)      ++gaps;
    else if (dt == 0)      ++dups;
  }

  out["gaps"]   = gaps;
  out["dups"]   = dups;
  out["ts_min"] = ts_min;
  out["ts_max"] = ts_max;
  return out;
}
