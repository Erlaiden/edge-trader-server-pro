#include "../httplib.h"
#include "json.hpp"
#include "../server_accessors.h"  // etai::{get_current_model,get_model_thr,get_model_ma_len}
#include "../utils_model.h"
#include "../utils_data.h"
#include <fstream>
#include <string>
#include <vector>
#include <optional>
#include <algorithm>

#if __has_include(<filesystem>)
  #include <filesystem>
  namespace fs = std::filesystem;
#else
  #error "C++17 filesystem is required"
#endif

// Агенты — опционально
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

// Безопасное чтение JSON-файла
static std::optional<json> read_json_file(const fs::path& p) {
    try {
        std::ifstream ifs(p);
        if (!ifs.good()) return std::nullopt;
        json j; ifs >> j;
        return j;
    } catch (...) {
        return std::nullopt;
    }
}

// Находим самый свежий *.json в cache/models/ (C++17: сравниваем file_time_type напрямую)
static std::optional<fs::path> latest_model_on_disk() {
    try {
        const fs::path dir = fs::path("cache") / "models";
        if (!fs::exists(dir) || !fs::is_directory(dir)) return std::nullopt;

        bool has_best = false;
        fs::file_time_type best_time{};
        fs::path best_path;

        for (auto it = fs::directory_iterator(dir);
             it != fs::directory_iterator(); ++it)
        {
            const auto& e = *it;
            if (!e.is_regular_file()) continue;
            const auto p = e.path();
            if (p.extension() != ".json") continue;

            fs::file_time_type ts;
            try { ts = fs::last_write_time(p); }
            catch (...) { continue; }

            if (!has_best || ts > best_time) {
                best_time = ts;
                best_path = p;
                has_best = true;
            }
        }
        if (!has_best) return std::nullopt;
        return best_path;
    } catch (...) {
        return std::nullopt;
    }
}

static void fill_from_ram(json& dst, const json& m) {
    auto put = [&](const char* k){ if (m.contains(k)) dst[k] = m[k]; };
    put("best_thr"); put("ma_len"); put("tp"); put("sl");
    put("version"); put("symbol"); put("interval"); put("schema"); put("mode");
    // feat_dim из policy
    try {
        if (m.contains("policy") && m["policy"].contains("feat_dim")) {
            dst["feat_dim"] = m["policy"]["feat_dim"];
        }
    } catch (...) {}
    // model_path (если есть)
    if (m.contains("model_path") && m["model_path"].is_string()) {
        dst["model_path"] = m["model_path"];
    }
}

static void fill_from_disk(json& dst) {
    // 1) путь из dst["model_path"], 2) иначе latest *.json
    std::optional<fs::path> mp;
    try {
        if (dst.contains("model_path") && dst["model_path"].is_string()) {
            fs::path p(dst["model_path"].get<std::string>());
            if (fs::exists(p) && fs::is_regular_file(p)) mp = p;
        }
    } catch (...) {}
    if (!mp) mp = latest_model_on_disk();
    if (!mp) return;

    auto j = read_json_file(*mp);
    if (!j) return;

    dst["model_path"] = mp->string();

    auto put_if_missing = [&](const char* k){
        if (!dst.contains(k) || dst[k].is_null()) {
            if (j->contains(k)) dst[k] = (*j)[k];
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
            && j->contains("policy") && (*j)["policy"].contains("feat_dim"))
        {
            dst["feat_dim"] = (*j)["policy"]["feat_dim"];
        }
    } catch (...) {}
}

void register_health_ai(httplib::Server& svr) {
    svr.Get("/api/health/ai", [](const httplib::Request&, httplib::Response& res) {
        json out;
        out["ok"] = true;

        // 1) Данные
        try {
            json dh = etai::get_data_health();
            out["data"] = dh;
            out["data_ok"] = dh.value("ok", false);
        } catch (...) {
            out["data"] = json{{"ok", false}};
            out["data_ok"] = false;
        }

        // 2) Модель
        json ms = null_model_short();
        try {
            json m = etai::get_current_model();
            fill_from_ram(ms, m);
        } catch (...) {
            // нет RAM-модели — не страшно
        }
        try {
            fill_from_disk(ms); // безопасная догрузка
        } catch (...) {}

        // аксессоры (не даём бросать исключения наружу)
        try { out["model_thr"]    = etai::get_model_thr(); }    catch (...) {}
        try { out["model_ma_len"] = etai::get_model_ma_len(); } catch (...) {}
        try {
            if (ms.contains("feat_dim") && !ms["feat_dim"].is_null()) {
                out["model_feat_dim"] = ms["feat_dim"];
            } else {
                out["model_feat_dim"] = nullptr;
            }
        } catch (...) {
            out["model_feat_dim"] = nullptr;
        }

        out["model"] = ms;

        // 3) Агенты
        json ag = json{
            {"ok", true},
            {"running", false},
            {"mode", "idle"},
            {"symbol", "BTCUSDT"},
            {"interval", "15"}
        };
        #if ETAI_HAS_AGENTS
        try { ag = etai::get_agent_public(); } catch (...) {}
        #endif
        out["agents"] = ag;

        res.status = 200;
        res.set_content(out.dump(2), "application/json");
    });
}
