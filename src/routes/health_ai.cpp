// health_ai: strict, deterministic model object.

#include "json.hpp"
#include "httplib.h"
#include <fstream>
#include <string>
#include <algorithm>

#include "server_accessors.h"  // etai::get_current_model(), get_model_thr(), get_model_ma_len(), get_data_health()
#include "utils_model.h"       // safe_read_json_file(), make_model()

using json = nlohmann::json;

namespace etai {
    json      get_current_model();
    double    get_model_thr();
    long long get_model_ma_len();
    json      get_data_health();
}

static inline json policy_stats_from(const json& disk){
    json out = json::object();
    if(!disk.is_object() || !disk.contains("policy")) return out;
    const json& P = disk["policy"];
    out["source"]   = disk.value("policy_source", std::string());
    out["feat_dim"] = P.value("feat_dim", 0);
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

void register_health_ai(httplib::Server& srv){
    auto handler = [](bool extended){
        return [extended](const httplib::Request&, httplib::Response& res){
            const double    thr = etai::get_model_thr();
            const long long ma  = etai::get_model_ma_len();
            const json disk     = etai::get_current_model();

            json out = json::object();
            out["ok"]           = true;
            out["model"]        = make_model(thr, ma, disk);
            out["model_thr"]    = thr;
            out["model_ma_len"] = ma;

            try{
                json d = etai::get_data_health();
                if(d.is_object()) out["data"] = d;
            }catch(...){}

            if(extended){
                out["train_telemetry"] = safe_read_json_file("cache/logs/last_train_telemetry.json");
            }

            out["policy_stats"] = policy_stats_from(disk);
            res.set_content(out.dump(2), "application/json");
        };
    };

    srv.Get("/api/health/ai", handler(false));
    srv.Get("/api/health/ai/extended", handler(true));
}
