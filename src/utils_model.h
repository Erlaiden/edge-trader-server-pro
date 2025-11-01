#pragma once
#include "json.hpp"
#include <string>
#include <fstream>
#include <type_traits>

using json = nlohmann::json;

// Безопасное чтение JSON из файла: {} при ошибке
static inline json safe_read_json_file(const std::string& p){
    std::ifstream f(p);
    if(!f.good()) return json::object();
    try{ json j; f >> j; return j; } catch(...){ return json::object(); }
}

// Принудительная числовая сериализация (исключаем NaN->null)
static inline json j_number(double v){
    if (!std::isfinite(v)) v = 0.0;
    return json::parse(std::to_string(v));
}
static inline json j_integer(long long v){
    if (v <= 0) v = 0;
    return json::parse(std::to_string(v));
}

// Сборка нормализованного объекта модели на основе атомов thr/ma и "диска".
// Прокидываем tp/sl/version, policy.feat_dim, symbol/interval, schema/mode/policy_source при наличии.
static inline json make_model(double thr, long long ma, const json& disk){
    json m = json::object();

    // Атомики в приоритете (детерминированно числом)
    m["best_thr"] = j_number(thr);
    m["ma_len"]   = j_integer(ma);

    // Базовые поля по умолчанию
    m["schema"]   = "ppo_pro_v1";
    m["mode"]     = "pro";

    // Если есть объект модели на диске — аккуратно подтягиваем поля
    if (disk.is_object()){
        // Топ-уровень
        if (disk.contains("schema")        && disk["schema"].is_string())      m["schema"]        = disk["schema"];
        if (disk.contains("mode")          && disk["mode"].is_string())        m["mode"]          = disk["mode"];
        if (disk.contains("policy_source") && disk["policy_source"].is_string()) m["policy_source"] = disk["policy_source"];
        if (disk.contains("version")       && disk["version"].is_number())     m["version"]       = disk["version"];
        if (disk.contains("tp")            && disk["tp"].is_number())          m["tp"]            = disk["tp"];
        if (disk.contains("sl")            && disk["sl"].is_number())          m["sl"]            = disk["sl"];

        // Символ/интервал
        if (disk.contains("symbol")   && disk["symbol"].is_string())   m["symbol"]   = disk["symbol"];
        if (disk.contains("interval") && disk["interval"].is_string()) m["interval"] = disk["interval"];
        if (!m.contains("symbol"))   m["symbol"]   = disk.value("symbol",   "BTCUSDT");
        if (!m.contains("interval")) m["interval"] = disk.value("interval", "15");

        // Policy и производные
        if (disk.contains("policy") && disk["policy"].is_object()) {
            m["policy"] = disk["policy"];
            if (disk["policy"].contains("feat_dim") && disk["policy"]["feat_dim"].is_number()){
                m["feat_dim"] = disk["policy"]["feat_dim"];
            }
        }
    }

    // Гарантируем «чистые» числа в best_thr/ma_len
    try { return json::parse(m.dump()); } catch (...) { return m; }
}
