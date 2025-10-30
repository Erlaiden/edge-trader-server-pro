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
static inline json make_model(double thr, long long ma, const json& disk){
    json m = json::object();
    m["best_thr"] = thr;
    m["ma_len"]   = ma;
    m["schema"]   = "ppo_pro_v1";
    m["mode"]     = "pro";

    if(disk.is_object()){
        if(disk.contains("policy"))        m["policy"]        = disk["policy"];
        if(disk.contains("policy_source")) m["policy_source"] = disk["policy_source"];
        if(disk.contains("symbol"))        m["symbol"]        = disk["symbol"];
        if(disk.contains("interval"))      m["interval"]      = disk["interval"];
        if(!m.contains("symbol"))   m["symbol"]   = disk.value("symbol",   "BTCUSDT");
        if(!m.contains("interval")) m["interval"] = disk.value("interval", "15");
    }
    try { return json::parse(m.dump()); } catch (...) { return m; }
}
