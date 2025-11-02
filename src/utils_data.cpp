#include "utils_data.h"
#include "utils.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <vector>
#include <algorithm>
#include <cctype>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace etai {

static inline std::string upper(std::string s){
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::toupper(c); });
  return s;
}

static inline std::string norm_interval(std::string s){
  auto canon = canonical_interval(s);
  if (canon.empty()) {
    return s;
  }
  return canon;
}

bool load_cached_xy(const std::string& symbol,
                    const std::string& interval,
                    arma::mat& X, arma::mat& y)
{
  try {
    const std::string xfile = "cache/xy/" + upper(symbol) + "_" + norm_interval(interval) + "_X.csv";
    const std::string yfile = "cache/xy/" + upper(symbol) + "_" + norm_interval(interval) + "_y.csv";
    if (fs::exists(xfile) && fs::exists(yfile)) {
      X.load(xfile, arma::csv_ascii);
      y.load(yfile, arma::csv_ascii);
      return (X.n_rows>0 && X.n_cols>0 && y.n_rows==X.n_rows);
    }
    arma::mat raw;
    if(!load_raw_ohlcv(symbol, interval, raw)) return false;

    arma::mat F(raw.n_rows, 8, arma::fill::zeros);
    arma::mat Y(raw.n_rows, 1, arma::fill::zeros);

    X = std::move(F);
    y = std::move(Y);

    fs::create_directories("cache/xy");
    X.save(xfile, arma::csv_ascii);
    y.save(yfile, arma::csv_ascii);
    return true;
  } catch (...) {
    return false;
  }
}

std::string select_raw_path(const std::string& symbol,
                            const std::string& interval,
                            bool& used_clean)
{
  used_clean = false;
  const std::string s = upper(symbol);
  const std::string tf = norm_interval(interval);

  const std::string p_clean = "cache/clean/" + s + "_" + tf + ".csv";
  const std::string p_raw   = "cache/"       + s + "_" + tf + ".csv";

  if (fs::exists(p_clean)) { used_clean = true; return p_clean; }
  return p_raw;
}

static inline void trim_to_6_cols(arma::mat& M){
  if (M.n_cols > 6) {
    M = M.cols(0,5);
  }
}

bool load_raw_ohlcv(const std::string& symbol,
                    const std::string& interval,
                    arma::mat& raw)
{
  try{
    bool used_clean = false;
    const std::string path = select_raw_path(symbol, interval, used_clean);
    if (!fs::exists(path)) {
      std::cerr << "[RAW] missing csv: " << path << std::endl;
      return false;
    }

    arma::mat M;
    if(!M.load(path, arma::csv_ascii)){
      std::cerr << "[RAW] failed to load csv: " << path << std::endl;
      return false;
    }

    trim_to_6_cols(M);

    if (M.n_cols != 6) {
      std::cerr << "[RAW] bad cols=" << M.n_cols << " (need 6) path=" << path << std::endl;
      return false;
    }

    if (M.n_rows < 300) {
      std::cerr << "[RAW] warn: rows=" << M.n_rows << " (<300) path=" << path << std::endl;
    }

    if (!used_clean && path.find("/cache/") != std::string::npos) {
      std::cerr << "[RAW] fallback used, trimmed to 6 cols from raw: " << path << std::endl;
    }

    raw = std::move(M);
    return true;
  }catch(const std::exception& e){
    std::cerr << "[RAW] exception: " << e.what() << std::endl;
    return false;
  }catch(...){
    std::cerr << "[RAW] unknown exception" << std::endl;
    return false;
  }
}

static json one_health(const std::string& symbol, const std::string& interval){
  json r;
  const std::string s = upper(symbol);
  const std::string tf = norm_interval(interval);

  const std::string p_clean = "cache/clean/" + s + "_" + tf + ".csv";
  const std::string p_raw   = "cache/"       + s + "_" + tf + ".csv";

  auto probe = [](const std::string& p)->json{
    json j{{"exists", fs::exists(p)}, {"path", p}, {"cols", 0}, {"rows", 0}};
    if (j["exists"].get<bool>()) {
      try{
        std::ifstream f(p);
        std::string line;
        if(std::getline(f, line)){
          int cols = 1 + std::count(line.begin(), line.end(), ',');
          j["cols"] = cols;
        }
        std::uintmax_t rows = 0;
        {
          std::ifstream fr(p);
          rows = std::count(std::istreambuf_iterator<char>(fr), {}, '\n');
        }
        j["rows"] = rows;
      }catch(...){}
    }
    return j;
  };

  r["symbol"]   = symbol;
  r["interval"] = tf;
  r["clean"]    = probe(p_clean);
  r["raw"]      = probe(p_raw);

  const int ccols = r["clean"]["cols"].get<int>();
  const std::uintmax_t crows = r["clean"]["rows"].get<std::uintmax_t>();
  const bool cex  = r["clean"]["exists"].get<bool>();

  bool ok = cex && ccols==6 && crows>=300;
  r["ok"] = ok;
  return r;
}

json data_health_report(const std::string& symbol, const std::string& interval){
  return one_health(symbol, interval);
}

json get_data_health(){
  json out;
  out["ok"] = true;
  out["data"] = json::array();
  for (auto tf : {"15","60","240","1440"}) {
    out["data"].push_back(one_health("BTCUSDT", tf));
  }
  return out;
}

} // namespace etai
