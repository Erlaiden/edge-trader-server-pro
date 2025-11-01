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

// Опционально подключаем агентов
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
        {"best_thr", nullptr},
        {"ma_len",   nullptr},
        {"tp",       nullptr},
        {"sl",       nullptr},
        {"feat_dim", nullptr},
        {"version",  nullptr},
        {"symbol",   nullptr},
        {"interval", nullptr},
        {"schema",   nullptr},
        {"mode",     nullptr},
        {"model_path", nullptr}
    };
}

// Безопасный доступ по пути a.b.c
static inline json jget_path(const json& j, std::initializer_list<const char*> path) {
    const json* cur = &j;
    for (auto k : path) {
        if (!cur->is_object() || !cur->contains(k)) return nullptr;
        cur = &((*cur)[k]);
    }
    return *cur;
}

static std::optional<json> load_json_file(const std::string& path) {
    try {
        std::ifstream ifs(path);
        if (!ifs.is_open()) return std::nullopt;
        json j; ifs >> j;
        return j;
    } catch (...) {
        return std::nullopt;
    }
}

static std::optional<std::string> find_latest_model_path(const std::string& dir = "cache/models") {
    try {
        if (!fs::exists(dir) || !fs::is_directory(dir)) return std::nullopt;
        std::vector<fs::directory_entry> files;
        for (const auto& e : fs::directory_iterator(dir)) {
            if (!e.is_regular_file()) continue;
            if (e.path().extension() == ".json") files.push_back(e);
        }
        if (files.empty()) return std::nullopt;
        std::sort(files.begin(), files.end(),
                  [](const fs::directory_entry& a, const fs::directory_entry& b){
                      return fs::last_write_time(a) > fs::last_write_time(b);
                  });
        return files.front().path().string();
    } catch (...) {
        return std::nullopt;
    }
}

static void fill_model_fields_from_json(json& dst_ms, const json& src) {
    auto set_if_null = [&](const char* k, const json& v){
        if (dst_ms[k].is_null() && !v.is_null()) dst_ms[k] = v;
    };

    set_if_null("best_thr", jget_path(src, {"best_thr"}));
    set_if_null("ma_len",   jget_path(src, {"ma_len"}));
    set_if_null("tp",       jget_path(src, {"tp"}));
    set_if_null("sl",       jget_path(src, {"sl"}));
    set_if_null("feat_dim", jget_path(src, {"feat_dim"})); // вдруг лежит плоско

    // Основной случай: policy.feat_dim
    json fd = jget_path(src, {"policy","feat_dim"});
    if (dst_ms["feat_dim"].is_null() && !fd.is_null()) dst_ms["feat_dim"] = fd;

    // Иногда TP/SL лежат под params.* или metrics.*
    if (dst_ms["tp"].is_null()) {
        json v = jget_path(src, {"params","tp"});
        if (v.is_null()) v = jget_path(src, {"metrics","tp"});
        set_if_null("tp", v);
    }
    if (dst_ms["sl"].is_null()) {
        json v = jget_path(src, {"params","sl"});
        if (v.is_null()) v = jget_path(src, {"metrics","sl"});
        set_if_null("sl", v);
    }

    // Часто встречающиеся метаданные
    set_if_null("version",  jget_path(src, {"version"}));
    set_if_null("symbol",   jget_path(src, {"symbol"}));
    set_if_null("interval", jget_path(src, {"interval"}));
    set_if_null("schema",   jget_path(src, {"schema"}));
    set_if_null("mode",     jget_path(src, {"mode"}));

    // Сохраним и путь к модели, если присутствует
    if (dst_ms["model_path"].is_null() && src.contains("model_path") && src["model_path"].is_string())
        dst_ms["model_path"] = src["model_path"];
}

void register_health_ai(httplib::Server& svr) {
    svr.Get("/api/health/ai", [](const httplib::Request&, httplib::Response& res) {
        json out;
        out["ok"] = true;

        // 1) Состояние данных
        try {
            json dh = etai::get_data_health();
            out["data"] = dh;
            out["data_ok"] = dh.value("ok", false);
        } catch (...) {
            out["data"] = json{{"ok", false}};
            out["data_ok"] = false;
        }

        // 2) Модель: сначала то, что есть в рантайме; затем — жёсткий диск
        json ms = null_model_short();
        json m_rt;
        bool have_rt = false;
        try {
            m_rt = etai::get_current_model();
            have_rt = true;
        } catch (...) {
            have_rt = false;
        }

        if (have_rt) {
            // Копируем основные верхнеуровневые поля из рантайма
            auto put = [&](const char* k){
                if (m_rt.contains(k)) ms[k] = m_rt[k];
            };
            put("best_thr"); put("ma_len"); put("tp"); put("sl");
            put("version");  put("symbol"); put("interval");
            put("schema");   put("mode");   put("model_path");

            // policy.feat_dim из рантайма
            try {
                if (m_rt.contains("policy") && m_rt["policy"].contains("feat_dim"))
                    ms["feat_dim"] = m_rt["policy"]["feat_dim"];
            } catch (...) {}
        }

        // Если всё ещё нет tp/sl/feat_dim — читаем модель с диска:
        // 1) если m_rt.model_path указан — пробуем его
        // 2) иначе берём самый свежий JSON из cache/models/
        std::optional<std::string> model_path;
        if (ms.contains("model_path") && ms["model_path"].is_string())
            model_path = ms["model_path"].get<std::string>();
        if (!model_path.has_value()) {
            // иногда путь лежит в m_rt["model"]["model_path"] на разных роутингах
            json mp = jget_path(m_rt, {"model_path"});
            if (mp.is_string()) model_path = mp.get<std::string>();
        }
        if (!model_path.has_value()) {
            model_path = find_latest_model_path("cache/models");
        }

        if (model_path.has_value()) {
            if (auto jdisk = load_json_file(*model_path)) {
                fill_model_fields_from_json(ms, *jdisk);
                // если в файле нет model_path — явно укажем, откуда читали
                if (ms["model_path"].is_null()) ms["model_path"] = *model_path;
            }
        }

        // Верхнеуровневые агрегаты (как и раньше)
        try { out["model_thr"]    = etai::get_model_thr(); }    catch (...) {}
        try { out["model_ma_len"] = etai::get_model_ma_len(); } catch (...) {}
        // model_feat_dim дублируем из ms
        out["model_feat_dim"] = ms.value("feat_dim", nullptr);

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
