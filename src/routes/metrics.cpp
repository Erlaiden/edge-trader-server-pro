// Prometheus metrics endpoint
#include "routes/metrics.h"
#include "server_accessors.h" // get_model_thr / get_model_ma_len / get_model_feat_dim / get_current_model
#include <sstream>

void register_metrics_routes(httplib::Server& srv)
{
  srv.Get("/metrics", [](const httplib::Request&, httplib::Response& res) {
    std::ostringstream m;

    // scalar gauges
    m << "# HELP edge_model_thr Model decision threshold\n"
      << "# TYPE edge_model_thr gauge\n"
      << "edge_model_thr " << etai::get_model_thr() << "\n";

    m << "# HELP edge_model_ma_len Model moving-average length\n"
      << "# TYPE edge_model_ma_len gauge\n"
      << "edge_model_ma_len " << etai::get_model_ma_len() << "\n";

    m << "# HELP edge_model_feat_dim Feature vector dimension of current policy\n"
      << "# TYPE edge_model_feat_dim gauge\n"
      << "edge_model_feat_dim " << etai::get_model_feat_dim() << "\n";

    // labels as 1-hot
    const auto& M = etai::get_current_model();
    if (M.is_object()) {
      std::string schema = M.value("schema", "");
      std::string mode   = M.value("mode",   "");
      std::string src    = M.value("policy_source", "learn");

      if (!schema.empty()) {
        m << "# HELP edge_model_schema Schema label of current model\n"
          << "# TYPE edge_model_schema gauge\n"
          << "edge_model_schema{schema=\"" << schema << "\"} 1\n";
      }
      if (!mode.empty()) {
        m << "# HELP edge_model_mode Mode label of current model\n"
          << "# TYPE edge_model_mode gauge\n"
          << "edge_model_mode{mode=\"" << mode << "\"} 1\n";
      }
      if (!src.empty()) {
        m << "# HELP edge_model_policy_source Policy source label\n"
          << "# TYPE edge_model_policy_source gauge\n"
          << "edge_model_policy_source{src=\"" << src << "\"} 1\n";
      }
    }

    res.set_content(m.str(), "text/plain; version=0.0.4");
  });
}
