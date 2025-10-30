// Prometheus metrics endpoint (stable, no extern atomics)
#include "routes/metrics.h"
#include "server_accessors.h"   // etai::get_model_thr(), get_model_ma_len(), get_current_model()
#include "json.hpp"
#include <sstream>
#include <cmath>
#include <algorithm>

using json = nlohmann::json;

static inline double clampd(double v, double lo, double hi){
    if(std::isnan(v) || !std::isfinite(v)) return lo;
    if(v < lo) return lo;
    if(v > hi) return hi;
    return v;
}

static inline int read_feat_dim_safe(){
    try{
        json m = etai::get_current_model();
        if(m.is_object() && m.contains("policy") && m["policy"].is_object()){
            return m["policy"].value("feat_dim", 0);
        }
    }catch(...){}
    return 0;
}

void register_metrics_routes(httplib::Server& srv){
    srv.Get("/metrics", [&](const httplib::Request&, httplib::Response& res){
        // читаем атомы через аксессоры и клампим
        double thr = 0.0006;
        long long ma = 12;
        try { thr = etai::get_model_thr(); } catch(...) {}
        try { ma  = etai::get_model_ma_len(); } catch(...) {}

        // разумный коридор для 15m BTC, но безопасен и для прочих
        thr = clampd(thr, 1e-4, 1e-2);

        std::ostringstream m;
        m << "# HELP edge_model_thr Model decision threshold\n# TYPE edge_model_thr gauge\n";
        m << "edge_model_thr " << thr << "\n";

        m << "# HELP edge_model_ma_len Model moving-average length\n# TYPE edge_model_ma_len gauge\n";
        m << "edge_model_ma_len " << ma << "\n";

        m << "# HELP edge_model_feat_dim Feature vector dimension of current policy\n# TYPE edge_model_feat_dim gauge\n";
        m << "edge_model_feat_dim " << read_feat_dim_safe() << "\n";

        // Лейблы: policy source & schema (one-hot gauges)
        try{
            json mdl = etai::get_current_model();
            std::string src = mdl.value("policy_source", "");
            if(!src.empty()){
                m << "# HELP edge_model_policy_source Policy source label\n# TYPE edge_model_policy_source gauge\n";
                m << "edge_model_policy_source{src=\"" << src << "\"} 1\n";
            }
            std::string schema = mdl.value("schema", "");
            if(!schema.empty()){
                m << "# HELP edge_model_schema Schema label of current model\n# TYPE edge_model_schema gauge\n";
                m << "edge_model_schema{schema=\"" << schema << "\"} 1\n";
            }
        }catch(...){}

        res.set_content(m.str(), "text/plain; version=0.0.4");
    });
}
