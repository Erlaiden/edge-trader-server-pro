// health_ai: strict, deterministic model object.
#include "json.hpp"
#include "httplib.h"
#include "server_accessors.h"   // etai::get_model_thr(), get_model_ma_len(), get_current_model()
#include "utils_data.h"         // etai::get_data_health()
#include <fstream>
#include <string>
#include <algorithm>

using json = nlohmann::json;

// safe file read
static inline json safe_read_json_file(const std::string& p){
    std::ifstream f(p);
    if(!f.good()) return json::object();
    try{ json j; f >> j; return j; } catch(...){ return json::object(); }
}

// extract compact policy stats
static inline json policy_stats_from(const json& disk){
    json out = json::object();
    if(!disk.is_object() || !disk.contains("policy")) return out;
    const json& P = disk["policy"];
    out["source"]    = disk.value("policy_source", std::string());
    out["feat_dim"]  = P.value("feat_dim", 0);
    if(P.contains("W") && P["W"].is_array()){
        const auto& W = P["W"];
        out["W_len"] = (unsigned)W.size();
        json head = json::array();
        for(size_t i=0;i<std::min<size_t>(8,W.size());++i) head.push_back(W[i]);
        out["W_head"] = head;
    }
    if(P.contains("b") && P["b"].is_array()) out["b_len"] = (unsigned)P["b"].size();
    return out;
}

// build strict model object
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
    }
    try { return json::parse(m.dump()); } catch (...) { return m; }
}

// route registrar
void register_health_ai(httplib::Server& srv){
    auto handler = [](bool extended){
        return [extended](const httplib::Request&, httplib::Response& res){
            const double     thr  = etai::get_model_thr();
            const long long  ma   = etai::get_model_ma_len();
            const json       disk = etai::get_current_model();

            json out = json::object();
            out["ok"]            = true;
            out["model"]         = make_model(thr, ma, disk);
            out["model_thr"]     = thr;
            out["model_ma_len"]  = ma;

            // data health
            try{
                json d = etai::get_data_health();
                if(d.is_object()){
                    if(!d.contains("ok")) d["ok"] = true;  // гарантируем .data.ok
                    out["data"] = d;
                }
            }catch(...){}

            if(extended){
                out["train_telemetry"] = safe_read_json_file("cache/logs/last_train_telemetry.json");
            }

            out["policy_stats"] = policy_stats_from(disk);
            res.set_content(out.dump(2), "application/json");
        };
    };

    srv.Get("/api/health/ai",           handler(false));
    srv.Get("/api/health/ai/extended",  handler(true));
}
