// Prometheus metrics endpoint
#include "routes/metrics.h"
#include "server_accessors.h"   // etai::get_model_thr(), get_model_ma_len(), get_current_model()
#include "json.hpp"
#include <sstream>
#include <string>

using json = nlohmann::json;

static inline std::string escape_label(const std::string& s){
    std::string out;
    out.reserve(s.size());
    for(char c : s){
        if(c=='\\' || c=='"') { out.push_back('\\'); out.push_back(c); }
        else if(c=='\n') { out += "\\n"; }
        else out.push_back(c);
    }
    return out;
}

void register_metrics_routes(httplib::Server& srv){
    srv.Get("/metrics", [](const httplib::Request&, httplib::Response& res){
        const double    thr = etai::get_model_thr();
        const long long ma  = etai::get_model_ma_len();
        const json      M   = etai::get_current_model();

        int feat_dim = 0;
        std::string policy_source = "unknown";
        std::string schema = "unknown";

        if(M.is_object()){
            if(M.contains("policy") && M["policy"].is_object()){
                feat_dim = M["policy"].value("feat_dim", 0);
            }
            policy_source = M.value("policy_source", policy_source);
            schema        = M.value("schema", schema);
        }

        std::ostringstream m;
        m.setf(std::ios::fixed); m.precision(10);

        m << "# HELP edge_model_thr Model decision threshold\n";
        m << "# TYPE edge_model_thr gauge\n";
        m << "edge_model_thr " << thr << "\n";

        m << "# HELP edge_model_ma_len Model moving-average length\n";
        m << "# TYPE edge_model_ma_len gauge\n";
        m << "edge_model_ma_len " << ma << "\n";

        m << "# HELP edge_model_feat_dim Feature vector dimension of current policy\n";
        m << "# TYPE edge_model_feat_dim gauge\n";
        m << "edge_model_feat_dim " << feat_dim << "\n";

        m << "# HELP edge_model_policy_source Policy source label\n";
        m << "# TYPE edge_model_policy_source gauge\n";
        m << "edge_model_policy_source{src=\"" << escape_label(policy_source) << "\"} 1\n";

        m << "# HELP edge_model_schema Schema label of current model\n";
        m << "# TYPE edge_model_schema gauge\n";
        m << "edge_model_schema{schema=\"" << escape_label(schema) << "\"} 1\n";

        res.set_content(m.str(), "text/plain; version=0.0.4");
    });
}
