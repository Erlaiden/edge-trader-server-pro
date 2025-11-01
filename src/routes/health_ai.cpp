#include "../httplib.h"
#include "json.hpp"
#include "../server_accessors.h"  // etai::{get_current_model,get_model_thr,get_model_ma_len}
#include "../utils_model.h"
#include "../utils_data.h"

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
        {"mode",     nullptr}
    };
}

// Аккуратный геттер по пути a.b.c с fallback’ом
static inline json jget_path(const json& j, std::initializer_list<const char*> path) {
    const json* cur = &j;
    for (auto k : path) {
        if (!cur->is_object() || !cur->contains(k)) return nullptr;
        cur = &((*cur)[k]);
    }
    return *cur;
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

        // 2) Модель (+ гарантированно заполняем tp/sl/feat_dim)
        json ms = null_model_short();
        try {
            const json m = etai::get_current_model();

            auto put_if_exists = [&](const char* k){
                if (m.contains(k)) ms[k] = m[k];
            };
            put_if_exists("best_thr");
            put_if_exists("ma_len");
            put_if_exists("tp");
            put_if_exists("sl");
            put_if_exists("version");
            put_if_exists("symbol");
            put_if_exists("interval");
            put_if_exists("schema");
            put_if_exists("mode");

            // feat_dim: policy.feat_dim -> model.feat_dim -> nullptr
            json fd = jget_path(m, {"policy","feat_dim"});
            if (fd.is_null() && m.contains("feat_dim")) fd = m["feat_dim"];
            ms["feat_dim"] = fd.is_null() ? nullptr : fd;

            // Доп. вычисляемые поля сверху (как раньше)
            try { out["model_thr"]    = etai::get_model_thr(); }    catch (...) {}
            try { out["model_ma_len"] = etai::get_model_ma_len(); } catch (...) {}

            // Если feat_dim не нашли в m, но вычислили выше — продублируем в ms
            try {
                if (ms["feat_dim"].is_null()) {
                    json fd2 = jget_path(m, {"policy","feat_dim"});
                    out["model_feat_dim"] = fd2.is_null() ? nullptr : fd2;
                    ms["feat_dim"] = out["model_feat_dim"];
                } else {
                    out["model_feat_dim"] = ms["feat_dim"];
                }
            } catch (...) {
                out["model_feat_dim"] = nullptr;
            }

            // Надёжные Fallback’и для tp/sl: ищем по альтернативным путям
            if (ms["tp"].is_null()) {
                json tp = jget_path(m, {"params","tp"});
                if (tp.is_null()) tp = jget_path(m, {"metrics","tp"});
                if (!tp.is_null()) ms["tp"] = tp;
            }
            if (ms["sl"].is_null()) {
                json sl = jget_path(m, {"params","sl"});
                if (sl.is_null()) sl = jget_path(m, {"metrics","sl"});
                if (!sl.is_null()) ms["sl"] = sl;
            }

        } catch (...) {
            // оставляем ms как есть (нулевые поля)
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
