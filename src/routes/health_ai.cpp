#include "../httplib.h"
#include "json.hpp"
#include "../server_accessors.h"  // etai::{get_current_model,get_model_thr,get_model_ma_len,get_model_feat_dim}
#include "../utils_model.h"
#include "../utils_data.h"
#include <fstream>
#include <string>
#include <optional>

#if __has_include(<filesystem>)
  #include <filesystem>
  namespace fs = std::filesystem;
#else
  #error "C++17 filesystem is required"
#endif

// Агенты — опционально (для SYMBOL/INTERVAL)
#if __has_include("../agents.h")
  #include "../agents.h"
  #define ETAI_HAS_AGENTS 1
#elif __has_include("agents.h")
  #include "agents.h"
  #define ETAI_HAS_AGENTS 1
#else
  #define ETAI_HAS_AGENTS 0
#endif

using json = nlohmann::json;

static inline json null_model_short() {
    return json{
        {"best_thr",   nullptr},
        {"ma_len",     nullptr},
        {"tp",         nullptr},
        {"sl",         nullptr},
        {"feat_dim",   nullptr},
        {"version",    nullptr},
        {"symbol",     nullptr},
        {"interval",   nullptr},
        {"schema",     nullptr},
        {"mode",       nullptr},
        {"model_path", nullptr}
    };
}

static std::optional<json> read_json_file(const fs::path& p) {
    try {
        std::ifstream ifs(p);
        if (!ifs.good()) return std::nullopt;
        json j; ifs >> j;
        return j;
    } catch (...) { return std::nullopt; }
}

// самый свежий *.json в cache/models/
static std::optional<fs::path> latest_model_on_disk() {
    try {
        const fs::path dir = fs::path("cache") / "models";
        if (!fs::exists(dir) || !fs::is_directory(dir)) return std::nullopt;
        std::optional<fs::path> best;
        fs::file_time_type best_ts{};
        for (const auto& e : fs::directory_iterator(dir)) {
            if (!e.is_regular_file()) continue;
            const auto& p = e.path();
            if (p.extension() != ".json") continue;
            fs::file_time_type ts;
            try { ts = fs::last_write_time(p); } catch (...) { continue; }
            if (!best || ts > best_ts) { best = p; best_ts = ts; }
        }
        return best;
    } catch (...) { return std::nullopt; }
}

static void fill_from_ram(json& dst, const json& m) {
    auto put = [&](const char* k){ if (m.contains(k)) dst[k] = m[k]; };
    put("best_thr"); put("ma_len"); put("tp"); put("sl");
    put("version"); put("symbol"); put("interval"); put("schema"); put("mode");
    try {
        if (m.contains("policy") && m["policy"].contains("feat_dim"))
            dst["feat_dim"] = m["policy"]["feat_dim"];
    } catch (...) {}
    if (m.contains("model_path") && m["model_path"].is_string())
        dst["model_path"] = m["model_path"];
}

static void fill_from_json(json& dst, const json& j, const fs::path& p) {
    dst["model_path"] = p.string();
    auto put_if_missing = [&](const char* k){
        if (!dst.contains(k) || dst[k].is_null()) {
            if (j.contains(k)) dst[k] = j[k];
        }
    };
    put_if_missing("best_thr");
    put_if_missing("ma_len");
    put_if_missing("tp");
    put_if_missing("sl");
    put_if_missing("version");
    put_if_missing("symbol");
    put_if_missing("interval");
    put_if_missing("schema");
    put_if_missing("mode");
    try {
        if ((!dst.contains("feat_dim") || dst["feat_dim"].is_null())
            && j.contains("policy") && j["policy"].contains("feat_dim"))
            dst["feat_dim"] = j["policy"]["feat_dim"];
    } catch (...) {}
}

static std::optional<fs::path> expected_model_path(const std::string& symbol, const std::string& interval) {
    // формируем cache/models/<SYMBOL>_<INTERVAL>_ppo_pro.json
    fs::path p = fs::path("cache") / "models" / (symbol + "_" + interval + "_ppo_pro.json");
    if (fs::exists(p) && fs::is_regular_file(p)) return p;
    return std::nullopt;
}

void register_health_ai(httplib::Server& svr) {
    svr.Get("/api/health/ai", [](const httplib::Request&, httplib::Response& res) {
        json out; out["ok"] = true;

        // 1) Данные
        try {
            json dh = etai::get_data_health();
            out["data"] = dh;
            out["data_ok"] = dh.value("ok", false);
        } catch (...) {
            out["data"] = json{{"ok", false}};
            out["data_ok"] = false;
        }

        // Получим предполагаемый SYMBOL/INTERVAL для точного пути
        std::string sym = "BTCUSDT";
        std::string ivl = "15";
        #if ETAI_HAS_AGENTS
        try {
            auto ag = etai::get_agent_public();
            if (ag.contains("symbol")   && ag["symbol"].is_string())   sym = ag["symbol"].get<std::string>();
            if (ag.contains("interval") && ag["interval"].is_string()) ivl = ag["interval"].get<std::string>();
        } catch (...) {}
        #endif

        // 2) Модель
        json ms = null_model_short();
        try { json m = etai::get_current_model(); fill_from_ram(ms, m); } catch (...) {}

        // (a) если RAM пустой — пробуем точный ожидаемый файл
        bool need_disk = (!ms.contains("tp") || ms["tp"].is_null()
                       || !ms.contains("sl") || ms["sl"].is_null()
                       || !ms.contains("ma_len") || ms["ma_len"].is_null()
                       || !ms.contains("best_thr") || ms["best_thr"].is_null()
                       || !ms.contains("feat_dim") || ms["feat_dim"].is_null());

        if (need_disk) {
            try {
                if (auto ep = expected_model_path(sym, ivl)) {
                    if (auto j = read_json_file(*ep)) fill_from_json(ms, *j, *ep);
                }
            } catch (...) {}
        }

        // (b) если всё ещё пусто — берём самый свежий *.json
        need_disk = (!ms.contains("tp") || ms["tp"].is_null()
                  || !ms.contains("sl") || ms["sl"].is_null()
                  || !ms.contains("ma_len") || ms["ma_len"].is_null()
                  || !ms.contains("best_thr") || ms["best_thr"].is_null()
                  || !ms.contains("feat_dim") || ms["feat_dim"].is_null());
        if (need_disk) {
            try {
                if (auto lp = latest_model_on_disk()) {
                    if (auto j = read_json_file(*lp)) fill_from_json(ms, *j, *lp);
                }
            } catch (...) {}
        }

        // Аксессоры: thr, ma_len
        try {
            auto thr = etai::get_model_thr();
            out["model_thr"] = thr;
            if (!ms.contains("best_thr") || ms["best_thr"].is_null()) ms["best_thr"] = thr;
        } catch (...) {}
        try {
            auto ml = etai::get_model_ma_len();
            out["model_ma_len"] = ml;
            if (!ms.contains("ma_len") || ms["ma_len"].is_null()) ms["ma_len"] = ml;
        } catch (...) {}

        // feat_dim — читаем из атомика, затем дублируем в модель при отсутствии
        try {
            auto fd = etai::get_model_feat_dim();
            out["model_feat_dim"] = fd;
            if (!ms.contains("feat_dim") || ms["feat_dim"].is_null()) ms["feat_dim"] = fd;
        } catch (...) {
            out["model_feat_dim"] = nullptr;
        }

        out["model"] = ms;

        // 3) Агенты
        json ag = json{
            {"ok", true}, {"running", false}, {"mode", "idle"},
            {"symbol", sym}, {"interval", ivl}
        };
        #if ETAI_HAS_AGENTS
        try { ag = etai::get_agent_public(); } catch (...) {}
        #endif
        out["agents"] = ag;

        res.status = 200;
        res.set_content(out.dump(2), "application/json");
    });
}
