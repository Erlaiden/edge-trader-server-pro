#include "utils_data.h"
#include "json.hpp"
#include <armadillo>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <vector>
#include <unordered_map>
#include <cmath>

using nlohmann::json;
namespace fs = std::filesystem;

namespace etai {

// ---------- helpers ----------
static inline bool split_csv_line(const std::string& s, std::vector<std::string>& out) {
    out.clear();
    std::string cur;
    std::istringstream is(s);
    while (std::getline(is, cur, ',')) out.push_back(cur);
    return !out.empty();
}

static inline bool to_double(const std::string& s, double& v) {
    try { v = std::stod(s); return true; } catch (...) { return false; }
}

static inline double sma_last(const std::vector<double>& a, size_t i, int W) {
    if ((int)i + 1 < W) return NAN;
    double sum = 0.0;
    for (int k = 0; k < W; ++k) sum += a[i - k];
    return sum / W;
}

static inline double pct(double a, double b) {
    if (std::isfinite(b) && std::fabs(b) > 1e-12) return (a - b) / b;
    return 0.0;
}

// ---------- data_health_report ----------
json data_health_report(const std::string& symbol, const std::string& interval) {
    const std::string path = "cache/" + symbol + "_" + interval + ".csv";
    json j;
    j["symbol"]   = symbol;
    j["interval"] = interval;
    j["ok"]       = fs::exists(path);
    if (!j["ok"]) return j;

    std::ifstream in(path);
    int rows = 0;
    std::string line;
    while (std::getline(in, line)) rows++;
    j["rows"]  = rows > 0 ? rows - 1 : 0;
    j["ts_min"] = nullptr;
    j["ts_max"] = nullptr;
    j["dups"]   = 0;
    j["gaps"]   = 0;
    return j;
}

// ---------- feature builder from base CSV ----------
static bool build_xy_from_base_csv(const std::string& symbol,
                                   const std::string& interval,
                                   arma::mat& X,
                                   arma::mat& y) {
    const std::string path = "cache/" + symbol + "_" + interval + ".csv";
    if (!fs::exists(path)) return false;

    std::ifstream f(path);
    if (!f) return false;

    std::string line;
    if (!std::getline(f, line)) return false;

    std::vector<std::string> cols;
    split_csv_line(line, cols);
    std::unordered_map<std::string,int> idx;
    for (int i=0;i<(int)cols.size();++i) {
        std::string k = cols[i];
        for (auto& c : k) c = std::tolower(c);
        idx[k] = i;
    }
    auto need = [&](const char* k){ return idx.count(k) ? idx[k] : -1; };
    int i_ts = need("ts"), i_o = need("open"), i_h = need("high"),
        i_l = need("low"), i_c = need("close"), i_v = need("volume");
    if (i_ts<0 || i_o<0 || i_h<0 || i_l<0 || i_c<0 || i_v<0) {
        f.clear(); f.seekg(0);
        i_ts = 0; i_o = 1; i_h = 2; i_l = 3; i_c = 4; i_v = 5;
        // повторно прочитаем первую строку как данные
        if (!std::getline(f, line)) return false;
    }

    std::vector<double> close, vol;
    std::vector<std::string> parts;
    do {
        if (line.empty()) continue;
        if (!split_csv_line(line, parts)) continue;
        if ((int)parts.size() <= std::max({i_ts,i_o,i_h,i_l,i_c,i_v})) continue;
        double c=0.0, vv=0.0;
        if (!to_double(parts[i_c], c)) continue;
        if (!to_double(parts[i_v], vv)) vv = 0.0;
        close.push_back(c);
        vol.push_back(vv);
    } while (std::getline(f, line));

    const size_t N = close.size();
    const int warmup = 26;
    if (N <= (size_t)(warmup+1)) return false;

    const size_t M = N - warmup - 1;
    X.set_size(M, 8);
    y.set_size(M, 1);

    for (size_t i = warmup; i + 1 < N; ++i) {
        size_t row = i - warmup;

        double c   = close[i];
        double c1  = close[i-1];
        double c2  = close[i-2];
        double c5  = close[i-5];

        double r1      = pct(c, c1);
        double r1_prev = pct(c1, c2);
        double r5      = pct(c, c5);

        double ma5  = sma_last(close, i, 5);
        double ma12 = sma_last(close, i, 12);
        double ma26 = sma_last(close, i, 26);

        double ma20v = sma_last(vol, i, 20);
        double vol_ch = pct(vol[i], vol[i-1]);
        double ma5n  = std::isfinite(ma5)  ? c/ma5  - 1.0 : 0.0;
        double ma12n = std::isfinite(ma12) ? c/ma12 - 1.0 : 0.0;
        double ma26n = std::isfinite(ma26) ? c/ma26 - 1.0 : 0.0;
        double vol_ma20n = std::isfinite(ma20v) ? vol[i]/ma20v - 1.0 : 0.0;

        X(row,0) = r1;
        X(row,1) = ma5n;
        X(row,2) = ma12n;
        X(row,3) = ma26n;
        X(row,4) = r1_prev;
        X(row,5) = r5;
        X(row,6) = vol_ma20n;
        X(row,7) = vol_ch;

        double next_ret = pct(close[i+1], c);
        y(row,0) = next_ret > 0.0 ? 1.0 : 0.0;
    }
    return true;
}

// ---------- public API ----------

// Загрузка кэша X/y; если нет — строим из base CSV и сохраняем
bool load_cached_xy(const std::string& symbol,
                    const std::string& interval,
                    arma::mat& X,
                    arma::mat& y) {
    const std::string xfile = "cache/X_" + symbol + "_" + interval + ".csv";
    const std::string yfile = "cache/y_" + symbol + "_" + interval + ".csv";

    auto try_load = [&]() -> bool {
        if (!fs::exists(xfile) || !fs::exists(yfile)) return false;
        try {
            X.load(xfile, arma::csv_ascii);
            y.load(yfile, arma::csv_ascii);
            return X.n_rows == y.n_rows && X.n_rows > 0 && X.n_cols > 0;
        } catch (...) { return false; }
    };

    if (try_load()) return true;
    if (!build_xy_from_base_csv(symbol, interval, X, y)) return false;

    try { X.save(xfile, arma::csv_ascii); y.save(yfile, arma::csv_ascii); } catch (...) {}
    return true;
}

// Новый: чтение сырого OHLCV (ts,open,high,low,close,volume) в arma::mat
bool load_raw_ohlcv(const std::string& symbol,
                    const std::string& interval,
                    arma::mat& raw)
{
    const std::string path = "cache/" + symbol + "_" + interval + ".csv";
    if (!fs::exists(path)) return false;

    std::ifstream f(path);
    if (!f) return false;

    std::string line;
    if (!std::getline(f, line)) return false;

    std::vector<std::string> cols;
    split_csv_line(line, cols);
    std::unordered_map<std::string,int> idx;
    for (int i=0;i<(int)cols.size();++i) {
        std::string k = cols[i];
        for (auto& c : k) c = std::tolower(c);
        idx[k] = i;
    }
    auto need = [&](const char* k){ return idx.count(k) ? idx[k] : -1; };
    int i_ts = need("ts"), i_o = need("open"), i_h = need("high"),
        i_l = need("low"), i_c = need("close"), i_v = need("volume");

    bool header_ok = (i_ts>=0 && i_o>=0 && i_h>=0 && i_l>=0 && i_c>=0 && i_v>=0);
    if (!header_ok) {
        // заголовка нет — читаем весь файл с фиксированными индексами
        f.clear(); f.seekg(0);
    }

    std::vector<std::array<double,6>> rows;
    std::vector<std::string> parts;
    do {
        if (line.empty()) continue;
        if (!split_csv_line(line, parts)) continue;

        // если нет заголовка — ожидаем 6 столбцов строго по порядку
        if (!header_ok) {
            if ((int)parts.size() < 6) continue;
            double ts=0,o=0,h=0,l=0,c=0,v=0;
            if (!to_double(parts[0], ts)) continue;
            to_double(parts[1], o); to_double(parts[2], h); to_double(parts[3], l);
            to_double(parts[4], c); to_double(parts[5], v);
            rows.push_back({ts,o,h,l,c,v});
        } else {
            if ((int)parts.size() <= std::max({i_ts,i_o,i_h,i_l,i_c,i_v})) continue;
            double ts=0,o=0,h=0,l=0,c=0,v=0;
            if (!to_double(parts[i_ts], ts)) continue;
            to_double(parts[i_o], o); to_double(parts[i_h], h); to_double(parts[i_l], l);
            to_double(parts[i_c], c); to_double(parts[i_v], v);
            rows.push_back({ts,o,h,l,c,v});
        }
    } while (std::getline(f, line));

    if (rows.size() < 300) return false;

    const size_t N = rows.size();
    raw.set_size(N, 6);
    for (size_t i=0;i<N;i++) {
        raw(i,0) = rows[i][0];
        raw(i,1) = rows[i][1];
        raw(i,2) = rows[i][2];
        raw(i,3) = rows[i][3];
        raw(i,4) = rows[i][4];
        raw(i,5) = rows[i][5];
    }
    return true;
}

// Агрегированный health по файлам в cache/
json get_data_health() {
    json out = json::object();
    try {
        if (!fs::exists("cache")) {
            out["ok"] = false;
            out["error"] = "cache_dir_missing";
            return out;
        }
        for (const auto& p : fs::directory_iterator("cache")) {
            if (!p.is_regular_file()) continue;
            if (p.path().extension().string() != ".csv") continue;
            const std::string fname = p.path().filename().string();
            json f;
            f["ok"]   = fs::exists(p.path());
            f["size"] = f["ok"] ? static_cast<uint64_t>(fs::file_size(p.path())) : 0ULL;
            try {
                std::ifstream in(p.path());
                std::string line;
                int rows = 0;
                while (std::getline(in, line)) rows++;
                f["rows"] = rows > 0 ? rows - 1 : 0;
            } catch (...) {
                f["rows"] = 0;
            }
            out[fname] = f;
        }
        out["ok"] = true;
    } catch (const std::exception& e) {
        out["ok"] = false;
        out["error"] = e.what();
    }
    return out;
}

} // namespace etai
