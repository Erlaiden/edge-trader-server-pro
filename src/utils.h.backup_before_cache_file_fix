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
#include <charconv>
#include <optional>
#include <string_view>
#include <cctype>
#include <sys/stat.h>
#include <sys/types.h>

namespace etai {

using json = nlohmann::json;

// ===== БАЗОВЫЕ ХЕЛПЕРЫ =====
inline void ensure_dir(const std::string& path) { ::mkdir(path.c_str(), 0755); }

// Нормализация интервала к каноническому виду файлов/расчётов
inline std::string canonical_interval(std::string interval) {
  interval.erase(std::remove_if(interval.begin(), interval.end(), [](unsigned char c){
    return std::isspace(c) || c=='\"' || c=='\'';
  }), interval.end());
  // допустимые формы → канон
  if (interval == "15m"   || interval == "15")                    return "15";
  if (interval == "60m"   || interval == "1h"  || interval=="60") return "60";
  if (interval == "240m"  || interval == "4h"  || interval=="240")return "240";
  if (interval == "1440m" || interval == "1d"  || interval=="D" || interval=="1440") return "1440";
  return interval;
}

inline std::string cache_file(const std::string& symbol, const std::string& interval) {
  return "cache/" + symbol + "_" + canonical_interval(interval) + ".csv";
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
inline int minutes_of(const std::string& interval){
  const std::string tf = canonical_interval(interval);
  if (tf=="15")   return 15;
  if (tf=="60")   return 60;
  if (tf=="240")  return 240;
  if (tf=="1440") return 1440;
  return 60;
}

inline long long tf_ms(const std::string& interval) {
  return (long long)minutes_of(interval) * 60LL * 1000LL;
}

// Bybit ожидает 'D' для дневки. Всё остальное — как есть.
inline std::string bybit_interval_param(const std::string& interval) {
  const std::string tf = canonical_interval(interval);
  if (tf == "1440") return "D";
  return tf;
}

// ===== Безопасный парсинг timestamp =====
inline std::optional<long long> parse_timestamp_token(std::string_view token) {
  // trim left spaces
  while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front()))) {
    token.remove_prefix(1);
  }
  // пропустить BOM
  if (token.size() >= 3 &&
      static_cast<unsigned char>(token[0]) == 0xEF &&
      static_cast<unsigned char>(token[1]) == 0xBB &&
      static_cast<unsigned char>(token[2]) == 0xBF) {
    token.remove_prefix(3);
  }
  // взять только ведущие цифры
  auto end = std::find_if_not(token.begin(), token.end(), [](unsigned char c){
    return std::isdigit(c);
  });
  if (end == token.begin()) return std::nullopt; // не начинается с цифры
  const char* begin_ptr = token.data();
  const char* end_ptr   = begin_ptr + (end - token.begin());
  long long value = 0;
  auto res = std::from_chars(begin_ptr, end_ptr, value);
  if (res.ec != std::errc()) return std::nullopt;
  return value;
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
    if (pos == std::string::npos) continue;
    auto ts = parse_timestamp_token(std::string_view(line.data(), pos));
    if (!ts) { ++skipped; continue; }
    out[*ts] = line;
  }
  return skipped;
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
                              std::vector<std::array<std::string,7>>& out_rows,
                              size_t* skipped_rows = nullptr) {
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

  std::vector<std::pair<long long, std::array<std::string,7>>> batch;
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
    auto ts = parse_timestamp_token(r[0]);
    if (!ts) {
      if (skipped_rows) ++(*skipped_rows);
      continue;
    }
    batch.emplace_back(*ts, std::move(r));
  }

  std::sort(batch.begin(), batch.end(), [](const auto& a, const auto& b){
    return a.first < b.first;
  });

  for (auto& item : batch) {
    out_rows.emplace_back(std::move(item.second));
  }
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
  size_t skipped_rows = 0;

  while (cursor < now_ms) {
    long long end_ms = std::min(cursor + frame * 1000, now_ms);
    if (!bybit_fetch_batch(cli, category, symbol, interval, cursor, end_ms, rows, &skipped_rows)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      continue;
    }
    if (rows.empty()) {
      cursor += frame * 1000;
      continue;
    }
    // найти последний валидный ts
    auto it = std::find_if(rows.rbegin(), rows.rend(), [](const auto& r){
      return parse_timestamp_token(r[0]).has_value();
    });
    if (it == rows.rend()) { ++skipped_rows; cursor += frame; continue; }
    long long new_last = *parse_timestamp_token((*it)[0]);
    if (new_last <= last_ts_seen) { cursor += frame; }
    else { last_ts_seen = new_last; cursor = last_ts_seen + frame; }
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
  }

  // merge → cache
  std::map<long long, std::string> merged;
  const auto path = cache_file(symbol, interval);
  skipped_rows += read_cache(path, merged);

  for (const auto& r : rows) {
    auto ts = parse_timestamp_token(r[0]);
    if (!ts) { ++skipped_rows; continue; }
    const long long ts_value = *ts;
    std::vector<std::string> cols{ r[0], r[1], r[2], r[3], r[4], r[5], r[6] };
    merged[ts_value] = join_csv(cols);
  }

  // trim N месяцев
  if (!merged.empty()) {
    std::map<long long, std::string> trimmed;
    for (auto it2 = merged.lower_bound(since_ms); it2 != merged.end(); ++it2)
      trimmed.emplace(it2->first, it2->second);
    merged.swap(trimmed);
  }

  write_cache(path, merged);

  if (skipped_rows > 0) {
    return json{
      {"ok", false},
      {"symbol", symbol},
      {"interval", canonical_interval(interval)},
      {"months", months},
      {"rows", (int)merged.size()},
      {"error", "invalid_timestamp"},
      {"skipped_rows", skipped_rows}
    };
  }

  return json{
    {"ok", true},
    {"symbol", symbol},
    {"interval", canonical_interval(interval)},
    {"months", months},
    {"rows", (int)merged.size()}
  };
}

// ===== LOAD MATRIX (нормализуем в 6×N: ts,open,high,low,close,volume) =====
inline arma::mat load_cached_matrix(const std::string& symbol, const std::string& interval) {
  const auto path = cache_file(symbol, interval);
  std::ifstream f(path);
  if (!f.good()) return arma::mat();

  std::vector<double> buf;
  buf.reserve(6 * 100000);

  std::string line;
  size_t nrows = 0;
  bool header_checked = false;

  while (std::getline(f, line)) {
    if (line.empty()) continue;

    // пропускаем заголовок, если он случайно попал
    if (!header_checked) {
      header_checked = true;
      // если первая ячейка не число — это точно заголовок
      std::string first = line.substr(0, line.find(','));
      bool digits = !first.empty() && std::all_of(first.begin(), first.end(), [](unsigned char c){ return std::isdigit(c); });
      if (!digits) continue;
    }

    // парсим CSV
    std::stringstream ss(line);
    std::string cell;
    std::vector<double> row;
    row.reserve(7);
    while (std::getline(ss, cell, ',')) {
      if (cell.empty()) { row.clear(); break; }
      try { row.push_back(std::stod(cell)); }
      catch (...) { row.clear(); break; }
    }
    if (row.empty()) continue;

    // ожидаем минимум 6 колонок: ts,o,h,l,c,vol; если 7 — игнорируем turnover
    if (row.size() < 6) continue;
    // только первые 6 полей
    for (int i=0;i<6;++i) buf.push_back(row[i]);
    ++nrows;
  }

  if (nrows == 0) return arma::mat();

  arma::mat M(6, nrows);
  for (size_t i = 0; i < nrows; ++i)
    for (size_t c = 0; c < 6; ++c)
      M(c, i) = buf[i*6 + c];
  return M;
}

} // namespace etai
