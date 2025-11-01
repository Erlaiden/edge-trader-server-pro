#include "httplib.h"
#include "json.hpp"
#include "utils_data.h"     // etai::get_data_health()
#include "utils_model.h"    // make_model(), j_number(), j_integer()
#include "agents.h"         // etai::get_agent_public()

using json = nlohmann::json;

// /api/health/ai — возвращает общее состояние модели, данных и агентов
static inline void register_health_ai(httplib::Server& svr) {
    svr.Get("/api/health/ai", [](const httplib::Request&, httplib::Response& res) {
        json out;
        out["ok"] = true;

        // Модель
        try {
            json disk = safe_read_json_file("cache/models/BTCUSDT_15_ppo_pro.json");
            out["model"] = make_model(0.3, 12, disk);
        } catch (...) {
            out["model"] = json{
                {"best_thr", nullptr}, {"ma_len", nullptr},
                {"tp", nullptr}, {"sl", nullptr},
                {"feat_dim", nullptr}, {"version", nullptr},
                {"symbol", nullptr}, {"interval", nullptr},
                {"schema", nullptr}, {"mode", nullptr}
            };
        }

        // Агент
        try {
            out["agents"] = etai::get_agent_public();
        } catch (...) {
            out["agents"] = json{
                {"symbol","BTCUSDT"},
                {"interval","15"},
                {"mode","idle"},
                {"running",false}
            };
        }

        // Проверка данных
        try {
            out["data"] = etai::get_data_health();
        } catch (...) {
            out["data"] = json{{"ok",false}};
        }

        res.set_content(out.dump(2), "application/json");
    });
}
