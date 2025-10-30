#include "server_accessors.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>

#include "rt_metrics.h"
#include "utils_data.h"
#include "json.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

// getenv с дефолтом
static std::string env_or(const char* key, const char* defval) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : std::string(defval);
}

static inline std::string model_path_of(const std::string& symbol, const std::string& interval) {
    return "cache/models/" + symbol + "_" + interval + "_ppo_pro.json";
}

template<typename T>
static T jnum(const json& j, const char* k, T dflt) {
    try {
        if (j.contains(k)) return j.at(k).get<T>();
    } catch (...) {}
    return dflt;
}

} // namespace

namespace etai {

json get_current_model() {
    const std::string symbol   = env_or("ETAI_SYMBOL",   "BTCUSDT");
    const std::string interval = env_or("ETAI_INTERVAL", "15");
    const std::string path     = model_path_of(symbol, interval);

    if (!fs::exists(path)) return json::object();

    try {
        std::ifstream f(path);
        if (!f) return json::object();
        json m = json::object();
        f >> m;
        return m;
    } catch (...) {
        return json::object();
    }
}

double get_model_thr() {
    return MODEL_BEST_THR.load(std::memory_order_relaxed);
}

long long get_model_ma_len() {
    return MODEL_MA_LEN.load(std::memory_order_relaxed);
}

json get_data_health() {
    const std::string symbol = env_or("ETAI_SYMBOL", "BTCUSDT");
    json out = json::object();
    out["symbol"] = symbol;

    try { out["m15"]   = data_health_report(symbol, "15");   } catch (...) { out["m15"]   = json{{"ok",false}}; }
    try { out["m60"]   = data_health_report(symbol, "60");   } catch (...) { out["m60"]   = json{{"ok",false}}; }
    try { out["m240"]  = data_health_report(symbol, "240");  } catch (...) { out["m240"]  = json{{"ok",false}}; }
    try { out["m1440"] = data_health_report(symbol, "1440"); } catch (...) { out["m1440"] = json{{"ok",false}}; }

    return out;
}

void init_model_atoms_from_disk() {
    json m = get_current_model();
    if (m.is_object() && !m.empty()) {
        const double    thr   = jnum<double>(m, "best_thr", 0.0);
        const long long ma    = jnum<long long>(m, "ma_len", 0);
        const long long bts   = jnum<long long>(m, "build_ts", 0);

        MODEL_BEST_THR.store(thr, std::memory_order_relaxed);
        MODEL_MA_LEN.store(ma,   std::memory_order_relaxed);
        MODEL_BUILD_TS.store(bts, std::memory_order_relaxed);
    }
}

} // namespace etai
