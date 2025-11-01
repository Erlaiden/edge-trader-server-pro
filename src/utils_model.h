#pragma once
#include "json.hpp"
#include <string>
#include <fstream>

using json = nlohmann::json;

// Безопасное чтение JSON из файла: {} при ошибке
static inline json safe_read_json_file(const std::string& p){
    std::ifstream f(p);
    if(!f.good()) return json::object();
    try{ json j; f >> j; return j; } catch(...){ return json::object(); }
}

// Сборка нормализованного объекта модели на основе атомов thr/ma и "диска"
// Прокидываем также tp/sl и feat_dim (если есть), чтобы /api/health/ai имел полную картину.
static inline json make_model(double thr, long long ma, const json& disk){
    json m = json::object();
    m["best_thr"] = thr;
    m["ma_len"]   = ma;
    m["schema"]   = "ppo_pro_v1";
    m["mode"]     = "pro";

    if (disk.is_object()) {
        // Базовые поля из диска (не ломаем совместимость)
        if (disk.contains("policy"))        m["policy"]        = disk["policy"];
        if (disk.contains("policy_source")) m["policy_source"] = disk["policy_source"];
        if (disk.contains("symbol"))        m["symbol"]        = disk["symbol"];
        if (disk.contains("interval"))      m["interval"]      = disk["interval"];
        if (disk.contains("schema"))        m["schema"]        = disk["schema"];    // если в диске новая схема — уважаем её
        if (disk.contains("version"))       m["version"]       = disk["version"];

        // --- ВАЖНО: прокинуть trading targets и размерность признаков ---
        if (disk.contains("tp"))            m["tp"]            = disk["tp"];
        if (disk.contains("sl"))            m["sl"]            = disk["sl"];

        // feat_dim может лежать внутри policy
        try {
            if (disk.contains("policy") && disk["policy"].is_object() && disk["policy"].contains("feat_dim")) {
                m["feat_dim"] = disk["policy"]["feat_dim"];
            }
        } catch (...) {}
    }

    // Дефолты для symbol/interval, если их не было в диске
    if(!m.contains("symbol"))   m["symbol"]   = "BTCUSDT";
    if(!m.contains("interval")) m["interval"] = "15";

    // Возвращаем строго валидный JSON (без NaN)
    try { return json::parse(m.dump()); } catch (...) { return m; }
}
