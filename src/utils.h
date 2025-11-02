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
#include <charconv>
#include <cctype>
#include <mutex>

namespace etai {

using json = nlohmann::json;

// ===== БАЗОВЫЕ ХЕЛПЕРЫ =====
inline void ensure_dir(const std::string& path) { ::mkdir(path.c_str(), 0755); }

inline std::string cache_file(const std::string& symbol, const std::string& interval) {
  return "cache/" + symbol + "_" + interval + ".csv";
}

inline std::string join_csv(const std::vector<std::string>& cols) {
  std::ostringstream oss;
  for (size_t i = 0; i < cols.size(); ++i) {
    if (i) oss << ',';
    oss << cols[i];
  }
  return oss.str();
}

// ===== INTERVAL HELPERS =====
inline std::string canonical_interval(std::string_view raw) {
  std::string cleaned;
  cleaned.reserve(raw.size());
  for (unsigned char ch : raw) {
    if (std::isspace(ch) || ch == '"') continue;
    cleaned.push_back(static_cast<char>(std::toupper(ch)));
  }
  if (cleaned.empty()) return {};

  auto parse_number = [](std::string_view s, long long& out) -> bool {
    if (s.empty()) return false;
    const char* begin = s.data();
    const char* end = begin + s.size();
    auto res = std::from_chars(begin, end, out);
    return res.ec == std::errc{} && res.ptr == end;
  };

  long long minutes = 0;

  if (cleaned == "D" || cleaned == "1D" || cleaned == "1DAY") {
    minutes = 1440;
  } else if (cleaned == "1H" || cleaned == "H1" || cleaned == "1HR" || cleaned == "1HOUR") {
    minutes = 60;
  } else if (cleaned == "4H" || cleaned == "H4" || cleaned == "4HR" || cleaned == "4HOUR") {
    minutes = 240;
  } else if (!cleaned.empty() && cleaned.back() == 'M') {
    long long value = 0;
    if (parse_number(std::string_view(cleaned).substr(0, cleaned.size() - 1), value)) {
      minutes = value;
    }
  } else if (!cleaned.empty() && cleaned.back() == 'H') {
    long long hours = 0;
    if (parse_number(std::string_view(cleaned).substr(0, cleaned.size() - 1), hours)) {
      minutes = hours * 60;
    }
  } else if (!cleaned.empty() && cleaned.back() == 'D') {
    long long days = 0;
    if (parse_number(std::string_view(cleaned).substr(0, cleaned.size() - 1), days)) {
      minutes = days * 1440;
    }
  } else {
    long long direct = 0;
    if (parse_number(cleaned, direct)) {
      minutes = direct;
    }
  }

  if (minutes == 15 || minutes == 60 || minutes == 240 || minutes == 1440) {
    return std::to_string(minutes);
  }
  return {};
}

inline long long tf_ms(std::string_view interval) {
  const std::string canon = canonical_interval(interval);
  if (canon.empty()) {
    return 60ll * 60 * 1000;
  }
  long long minutes = 0;
  auto res = std::from_chars(canon.data(), canon.data() + canon.size(), minutes);
  if (res.ec != std::errc{}) {
    return 60ll * 60 * 1000;
  }
  return minutes * 60 * 1000;
}

// ===== TOKEN PARSING =====
inline bool parse_timestamp_token(std::string_view token, long long& out) {
  const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
  size_t i = 0;
  if (token.size() >= 3 && static_cast<unsigned char>(token[0]) == bom[0] &&
      static_cast<unsigned char>(token[1]) == bom[1] && static_cast<unsigned char>(token[2]) == bom[2]) {
    i = 3;
  }
  while (i < token.size() && std::isspace(static_cast<unsigned char>(token[i]))) {
    ++i;
  }
  const char* begin = token.data() + i;
  const char* end = token.data() + token.size();
  auto res = std::from_chars(begin, end, out);
  if (res.ec != std::errc{}) {
    return false;
  }
  while (res.ptr < end && std::isspace(static_cast<unsigned char>(*res.ptr))) {
    ++res.ptr;
  }
  return res.ptr == end;
}

// ===== CSV CACHE =====
inline size_t read_cache(const std::string& path, std::map<long long, std::string>& out) {
  std::ifstream f(path);
  if (!f.good()) return 0;
  std::string line;
  size_t skipped = 0;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    size_t pos = line.find(',');
    if (pos == std::string::npos) {
      ++skipped;
      continue;
    }
    long long ts = 0;
    if (!parse_timestamp_token(std::string_view(line.data(), pos), ts)) {
      ++skipped;
      continue;
    }
    out[ts] = line;
  }
  return skipped;
}

inline void write_cache(const std::string& path, const std::map<long long, std::string>& data) {
  std::ofstream f(path, std::ios::trunc);
  for (auto& kv : data) {
    f << kv.second << "\n";
  }
}

struct BackfillStats {
  bool ok = false;
  std::string error;
  std::string symbol;
  std::string interval;
  std::string canonical_interval;
  int months = 0;
  size_t rows = 0;
  size_t skipped_rows = 0;
  size_t fetched_rows = 0;

  json to_json() const {
    json j{
      {"ok", ok},
      {"symbol", symbol},
      {"interval", interval},
      {"interval_canonical", canonical_interval},
      {"months", months},
      {"rows", rows},
      {"skipped_rows", skipped_rows},
      {"fetched_rows", fetched_rows}
    };
    if (!error.empty()) {
      j["error"] = error;
    }
    return j;
  }
};

inline std::string bybit_interval_param(const std::string& interval) {
  if (interval == "1440") return "D";
  return interval;
}

inline bool bybit_fetch_batch(httplib::SSLClient& cli,
                              const std::string& category,
                              const std::string& symbol,
                              const std::string& interval,
                              long long start_ms,
                              long long end_ms,
                              std::vector<std::array<std::string,7>>& out_rows,
                              size_t& skipped_rows) {
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

  struct ParsedRow {
    long long ts;
    std::array<std::string,7> row;
  };
  std::vector<ParsedRow> batch;
  batch.reserve(arr.size());

  for (const auto& row : arr) {
    if (!row.is_array() || row.size() < 7) {
      ++skipped_rows;
      continue;
    }
    std::array<std::string,7> r{
      row.at(0).get<std::string>(),
      row.at(1).get<std::string>(),
      row.at(2).get<std::string>(),
      row.at(3).get<std::string>(),
      row.at(4).get<std::string>(),
      row.at(5).get<std::string>(),
      row.at(6).get<std::string>()
    };
    long long ts = 0;
    if (!parse_timestamp_token(r[0], ts)) {
      ++skipped_rows;
      continue;
    }
    batch.push_back({ts, std::move(r)});
  }

  std::sort(batch.begin(), batch.end(), [](const ParsedRow& a, const ParsedRow& b){
    return a.ts < b.ts;
  });

  for (auto& pr : batch) {
    out_rows.emplace_back(std::move(pr.row));
  }
  return true;
}

inline BackfillStats backfill_last_months(const std::string& symbol,
                                          const std::string& interval,
                                          int months = 6,
                                          const std::string& category = "linear") {
  BackfillStats stats;
  std::string symbol_upper = symbol;
  std::transform(symbol_upper.begin(), symbol_upper.end(), symbol_upper.begin(), [](unsigned char c){ return std::toupper(c); });
  stats.symbol = symbol_upper;
  stats.canonical_interval = canonical_interval(interval);
  stats.interval = stats.canonical_interval.empty() ? interval : stats.canonical_interval;
  stats.months = months;
  if (stats.canonical_interval.empty()) {
    stats.error = "invalid_interval";
    return stats;
  }

  try {
    ensure_dir("cache");

    const long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const long long span_ms = static_cast<long long>(months * 30.5 * 24 * 60 * 60 * 1000.0);
    const long long since_ms = now_ms - span_ms;

    httplib::SSLClient cli("api.bybit.com");
    cli.enable_server_certificate_verification(true);
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(20, 0);

    std::vector<std::array<std::string,7>> rows;
    rows.reserve(50000);

    long long cursor = since_ms;
    const long long frame = tf_ms(stats.canonical_interval);
    long long last_ts_seen = -1;
    size_t skipped_rows = 0;

    while (cursor < now_ms) {
      long long end_ms = std::min(cursor + frame * 1000, now_ms);
      size_t batch_skipped = 0;
      if (!bybit_fetch_batch(cli, category, symbol, stats.canonical_interval, cursor, end_ms, rows, batch_skipped)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        continue;
      }
      skipped_rows += batch_skipped;
      if (rows.empty()) {
        cursor += frame * 1000;
        continue;
      }
      long long new_last = -1;
      if (!parse_timestamp_token(rows.back()[0], new_last)) {
        ++skipped_rows;
        rows.pop_back();
        continue;
      }
      if (new_last <= last_ts_seen) {
        cursor += frame;
      } else {
        last_ts_seen = new_last;
        cursor = last_ts_seen + frame;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }

    std::map<long long, std::string> merged;
    const auto path = cache_file(symbol_upper, stats.canonical_interval);
    skipped_rows += read_cache(path, merged);

    for (const auto& r : rows) {
      long long ts = 0;
      if (!parse_timestamp_token(r[0], ts)) {
        ++skipped_rows;
        continue;
      }
      std::vector<std::string> cols{r[0], r[1], r[2], r[3], r[4], r[5], r[6]};
      merged[ts] = join_csv(cols);
    }

    if (!merged.empty()) {
      std::map<long long, std::string> trimmed;
      for (auto it = merged.lower_bound(since_ms); it != merged.end(); ++it) {
        trimmed.emplace(it->first, it->second);
      }
      merged.swap(trimmed);
    }

    write_cache(path, merged);

    ensure_dir("cache/clean");
    const auto clean_path = std::string("cache/clean/") + symbol_upper + "_" + stats.canonical_interval + ".csv";
    std::ofstream clean(clean_path, std::ios::trunc);
    size_t clean_rows = 0;
    for (const auto& kv : merged) {
      std::stringstream ss(kv.second);
      std::string cell;
      std::vector<std::string> cells;
      while (std::getline(ss, cell, ',')) {
        cells.push_back(cell);
      }
      if (cells.size() < 6) {
        ++skipped_rows;
        continue;
      }
      std::vector<std::string> six{cells.begin(), cells.begin() + 6};
      clean << join_csv(six) << "\n";
      ++clean_rows;
    }

    stats.ok = clean_rows > 0;
    stats.rows = clean_rows;
    stats.skipped_rows = skipped_rows;
    stats.fetched_rows = rows.size();
    return stats;
  } catch (const std::exception& e) {
    stats.error = e.what();
    return stats;
  } catch (...) {
    stats.error = "unknown_exception";
    return stats;
  }
}

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
      double v = 0;
      auto res = std::from_chars(cell.data(), cell.data() + cell.size(), v);
      if (res.ec != std::errc{}) {
        try {
          v = std::stod(cell);
        } catch (...) {
          v = 0.0;
        }
      }
      buf.push_back(v);
      ++col;
    }
    if (col == 7) ++nrows;
  }
  if (nrows == 0) return arma::mat();

  arma::mat M(7, nrows);
  for (size_t i = 0; i < nrows; ++i) {
    for (size_t c = 0; c < 7; ++c) {
      M(c, i) = buf[i * 7 + c];
    }
  }
  return M;
}

} // namespace etai
