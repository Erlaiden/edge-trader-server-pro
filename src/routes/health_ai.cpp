#include "../httplib.h"
#include "json.hpp"
#include "../server_accessors.h"  // etai::{get_current_model,get_model_thr,get_model_ma_len}
#include "../utils_model.h"       // совместимые хелперы, без дубликатов
#include "../utils_data.h"        // etai::get_data_health()

// Подключаем сведения об агентах, если заголовок реально существует.
// Проверяем два варианта путей: рядом и на уровень выше.
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
        {"mode",     nullptr}
    };
}

void register_health_ai(httplib::Server& svr) {
    svr.Get("/api/health/ai", [](const httplib::Request&, httplib::Response& res) {
        json out;
        out["ok"] = true;

        // 1) Данные (только локальные CSV)
        try {
            json dh = etai::get_data_health();
            out["data"] = dh;
            out["data_ok"] = dh.value("ok", false);
        } catch (...) {
            out["data"] = json{{"ok", false}};
            out["data_ok"] = false;
        }

        // 2) Модель (как в /api/model)
        json ms = null_model_short();
        try {
            json m = etai::get_current_model();

            auto put = [&](const char* k){
                if (m.contains(k)) ms[k] = m[k];
            };
            put("best_thr");
            put("ma_len");
            put("tp");
            put("sl");
            put("version");
            put("symbol");
            put("interval");
            put("schema");
            put("mode");

            try {
                if (m.contains("policy") && m["policy"].contains("feat_dim")) {
                    ms["feat_dim"] = m["policy"]["feat_dim"];
                }
            } catch (...) {}

            try { out["model_thr"]      = etai::get_model_thr(); }      catch (...) {}
            try { out["model_ma_len"]   = etai::get_model_ma_len(); }   catch (...) {}
            try {
                out["model_feat_dim"] = (m.contains("policy") && m["policy"].contains("feat_dim"))
                                         ? m["policy"]["feat_dim"] : nullptr;
            } catch (...) {}
        } catch (...) {
            // оставляем ms пустым
        }
        out["model"] = ms;

        // 3) Агенты (безопасно: только если заголовок найден)
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
