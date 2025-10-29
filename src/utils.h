#pragma once
#include "httplib.h"
#include "json.hpp"

#include <armadillo>
#include <string>
#include <vector>
#include <array>
#include <map>
#include <chrono>
#include <thread>
#include <fstream>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <sys/stat.h>
#include <sys/types.h>

namespace etai {

using json = nlohmann::json;

// ===== БАЗОВЫЕ ХЕЛПЕРЫ =====
inline void ensure_dir(const std::string& path) { ::mkdir(path.c_str(), 0755); }

inline std::string cache_file(const std::string& symbol, const std::string& interval) {
  return "cache/" + symbol + "_" + interval + ".csv";
}

inline std::string join_csv(const std::vector<std::string>& cols) {
  std::ostringstream oss;
  for (size_t i=0;i<cols.size();++i) { if (i) oss << ','; oss << cols[i]; }
  return oss.str();
}

// ===== URL/QUERY =====
inline std::string urlencode(const std::string& s) {
  std::ostringstream oss;
  oss.fill('0');
  oss << std::hex << std::uppercase;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c=='-' || c=='_' || c=='.' || c=='~') { oss << c; }
    else if (c==' ') { oss << "%20"; }
    else { oss << '%' << std::setw(2) << int(c); }
  }
  return oss.str();
}

inline std::string make_query(const std::vector<std::pair<std::string,std::string>>& kvs) {
  std::ostringstream oss;
  for (size_t i=0;i<kvs.size();++i) {
    if (i) oss << '&';
    oss << urlencode(kvs[i].first) << '=' << urlencode(kvs[i].second);
  }
  return oss.str();
}

// ===== TIMEFRAME (с поддержкой 1440) =====
inline long long tf_ms(const std::string& interval) {
  if (interval=="15")   return 15ll   * 60 * 1000;
  if (interval=="60")   return 60ll   * 60 * 1000;
  if (interval=="240")  return 240ll  * 60 * 1000;
  if (interval=="1440") return 1440ll * 60 * 1000; // 1d
  return 60ll * 60 * 1000; // default 1h
}

// Bybit ожидает 'D' для дневки. Всё остальное — как есть.
inline std::string bybit_interval_param(const std::string& interval) {
  if (interval == "1440") return "D";
  return interval;
}

// ===== CSV CACHE =====
inline void read_cache(const std::string& path, std::map<long long, std::string>& out) {
  std::ifstream f(path);
  if (!f.good()) return;
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    size_t pos = line.find(',');
    if (pos == std::string::npos) continue;
    long long ts = std::stoll(line.substr(0, pos));
    out[ts] = line;
  }
}

inline void write_cache(const std::string& path, const std::map<long long, std::string>& data) {
  std::ofstream f(path, std::ios::trunc);
  for (auto& kv : data) f << kv.second << "\n";
}

// ===== BYBIT v5 /market/kline =====
inline bool bybit_fetch_batch(httplib::SSLClient& cli,
                              const std::string& category,
                              const std::string& symbol,
                              const std::string& interval,
                              long long start_ms,
                              long long end_ms,
                              std::vector<std::array<std::string,7>>& out_rows) {
  const std::string interval_param = bybit_interval_param(interval);
  std::vector<std::pair<std::string,std::string>> kv{
    {"category", category},
    {"symbol",   symbol},
    {"interval", interval_param},
    {"start",    std::to_string(start_ms)},
    {"end",      std::to_string(end_ms)},
    {"limit",    "1000"}
  };
  std::string path = "/v5/market/kline?" + make_query(kv);

  auto res = cli.Get(path.c_str());
  if (!res || res->status != 200) return false;

  auto j = json::parse(res->body, nullptr, false);
  if (j.is_discarded()) return false;
  if (!j.contains("retCode")) return false;
  if (j["retCode"].get<int>() != 0) return false;

  if (!j.contains("result") || !j["result"].contains("list")) return true;
  const auto& arr = j["result"]["list"];
  if (!arr.is_array()) return true;

  std::vector<std::array<std::string,7>> batch;
  batch.reserve(arr.size());
  for (const auto& row : arr) {
    if (!row.is_array() || row.size() < 7) continue;
    std::array<std::string,7> r{
      row.at(0).get<std::string>(),
      row.at(1).get<std::string>(),
      row.at(2).get<std::string>(),
      row.at(3).get<std::string>(),
      row.at(4).get<std::string>(),
      row.at(5).get<std::string>(),
      row.at(6).get<std::string>()
    };
    batch.emplace_back(std::move(r));
  }

  std::sort(batch.begin(), batch.end(), [](const auto& a, const auto& b){
    return std::stoll(a[0]) < std::stoll(b[0]);
  });

  out_rows.insert(out_rows.end(), batch.begin(), batch.end());
  return true;
}

// ===== BACKFILL (15/60/240/1440) =====
inline nlohmann::json backfill_last_months(const std::string& symbol,
                                           const std::string& interval,
                                           int months = 6,
                                           const std::string& category = "linear") {
  ensure_dir("cache");

  const long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  const long long span_ms = (long long)(months * 30.5 * 24 * 60 * 60 * 1000.0);
  const long long since_ms = now_ms - span_ms;

  httplib::SSLClient cli("api.bybit.com");
  cli.enable_server_certificate_verification(true);
  cli.set_connection_timeout(5, 0);
  cli.set_read_timeout(20, 0);

  std::vector<std::array<std::string,7>> rows;
  rows.reserve(50000);

  long long cursor = since_ms;
  const long long frame = tf_ms(interval);
  long long last_ts_seen = -1;

  while (cursor < now_ms) {
    long long end_ms = std::min(cursor + frame * 1000, now_ms);
    if (!bybit_fetch_batch(cli, category, symbol, interval, cursor, end_ms, rows)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      continue;
    }
    if (rows.empty()) {
      cursor += frame * 1000;
      continue;
    }
    long long new_last = std::stoll(rows.back()[0]);
    if (new_last <= last_ts_seen) { cursor += frame; }
    else { last_ts_seen = new_last; cursor = last_ts_seen + frame; }
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
  }

  // merge → cache
  std::map<long long, std::string> merged;
  const auto path = cache_file(symbol, interval);
  read_cache(path, merged);

  for (const auto& r : rows) {
    long long ts = std::stoll(r[0]);
    std::vector<std::string> cols{ r[0], r[1], r[2], r[3], r[4], r[5], r[6] };
    merged[ts] = join_csv(cols);
  }

  // trim N месяцев
  if (!merged.empty()) {
    std::map<long long, std::string> trimmed;
    for (auto it = merged.lower_bound(since_ms); it != merged.end(); ++it)
      trimmed.emplace(it->first, it->second);
    merged.swap(trimmed);
  }

  write_cache(path, merged);

  return json{
    {"ok", true},
    {"symbol", symbol},
    {"interval", interval},
    {"months", months},
    {"rows", (int)merged.size()}
  };
}

// ===== LOAD MATRIX (7 x N) =====
inline arma::mat load_cached_matrix(const std::string& symbol, const std::string& interval) {
  const auto path = cache_file(symbol, interval);
  std::ifstream f(path);
  if (!f.good()) return arma::mat();

  std::vector<double> buf;
  buf.reserve(7 * 50000);
  std::string line;
  size_t nrows = 0;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    std::stringstream ss(line);
    std::string cell;
    size_t col = 0;
    while (std::getline(ss, cell, ',')) {
      buf.push_back(std::stod(cell));
      ++col;
    }
    if (col == 7) ++nrows;
  }
  if (nrows == 0) return arma::mat();

  arma::mat M(7, nrows);
  for (size_t i = 0; i < nrows; ++i)
    for (size_t c = 0; c < 7; ++c)
      M(c, i) = buf[i*7 + c];

  return M;
}

} // namespace etai
