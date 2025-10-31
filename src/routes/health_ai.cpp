// health_ai: strict, deterministic model object (+ robust numeric serialization & self-heal).
#include "json.hpp"
#include "httplib.h"
#include "server_accessors.h"   // etai::{get/set_model_thr,get/set_model_ma_len,get/set_current_model}
#include "utils_data.h"         // etai::get_data_health()
#include "utils_model.h"        // safe_read_json_file(), make_model()
#include <cmath>
#include <algorithm>
#include <string>

using json = nlohmann::json;

static inline double     sanitize_thr(double x, double defv){ return std::isfinite(x) ? x : defv; }
static inline long long  sanitize_ma (long long x, long long defv){ return x > 0 ? x : defv; }
// Принудительная числовая сериализация (без NaN->null)
static inline json j_number(double v){ return json::parse(std::to_string(v)); }
static inline json j_integer(long long v){ return json::parse(std::to_string(v)); }

void register_health_ai(httplib::Server& srv){
    auto handler = [](bool extended){
        return [extended](const httplib::Request&, httplib::Response& res){
            // 1) читаем атомики
            double     thr_raw = etai::get_model_thr();
            long long  ma_raw  = etai::get_model_ma_len();
            json       disk    = etai::get_current_model();

            // 2) санитизация
            const double    thr = sanitize_thr(thr_raw, 0.38);
            const long long ma  = sanitize_ma (ma_raw,  12);

            // 3) самовосстановление состояния, если было NaN/некорректно
            if (!std::isfinite(thr_raw)) etai::set_model_thr(thr);
            if (!(ma_raw > 0))           etai::set_model_ma_len(ma);
            if (!disk.is_object())       etai::set_current_model(json::object());

            // 4) ответ (числа строго как numbers, не null)
            json out = json::object();
            out["ok"]            = true;
            out["model"]         = make_model(thr, ma, disk);
            out["model_thr"]     = j_number(thr);
            out["model_ma_len"]  = j_integer(ma);

            // data health (best-effort)
            try{
                json d = etai::get_data_health();
                if(d.is_object()){
                    if(!d.contains("ok")) d["ok"] = true;
                    out["data"] = d;
                }
            }catch(...){}

            // policy stats (коротко)
            auto policy_stats_from = [](const json& dj){
                json o = json::object();
                if(!dj.is_object() || !dj.contains("policy")) return o;
                const json& P = dj["policy"];
                o["source"]    = dj.value("policy_source", std::string());
                o["feat_dim"]  = P.value("feat_dim", 0);
                if(P.contains("W") && P["W"].is_array()){
                    const auto& W = P["W"];
                    o["W_len"] = (unsigned)W.size();
                    json head = json::array();
                    for(size_t i=0;i<std::min<size_t>(8,W.size());++i) head.push_back(W[i]);
                    o["W_head"] = head;
                }
                if(P.contains("b") && P["b"].is_array()) o["b_len"] = (unsigned)P["b"].size();
                return o;
            };
            out["policy_stats"] = policy_stats_from(disk);

            res.set_content(out.dump(2), "application/json");
        };
    };

    srv.Get("/api/health/ai",           handler(false));
    srv.Get("/api/health/ai/extended",  handler(true));
}
