#include <sstream>
#include <string>
#include <httplib.h>
#include "rt_metrics.h"       // REQ_METRICS (если есть), не критично
#include "server_accessors.h" // get_model_thr(), get_model_ma_len(), get_current_model()
#include "json.hpp"

using nlohmann::json;

static void register_metrics_route(httplib::Server& srv){
  srv.Get("/metrics", [](const httplib::Request&, httplib::Response& res){
    std::ostringstream m;

    // Базовые метрики модели
    m << "# HELP edge_model_thr Model decision threshold\n";
    m << "# TYPE edge_model_thr gauge\n";
    m << "edge_model_thr " << etai::get_model_thr() << "\n";

    m << "# HELP edge_model_ma_len Model moving-average length\n";
    m << "# TYPE edge_model_ma_len gauge\n";
    m << "edge_model_ma_len " << etai::get_model_ma_len() << "\n";

    // Попробуем взять feat_dim/schema/mode из текущей модели
    const json& model = etai::get_current_model();
    int feat_dim = 0;
    std::string schema, mode;
    if(model.is_object()){
      if(model.contains("policy") && model["policy"].is_object())
        feat_dim = model["policy"].value("feat_dim", 0);
      schema = model.value("schema", "");
      mode   = model.value("mode",   "");
    }

    m << "# HELP edge_model_feat_dim Feature vector dimension of current policy\n";
    m << "# TYPE edge_model_feat_dim gauge\n";
    m << "edge_model_feat_dim " << feat_dim << "\n";

    if(!schema.empty()){
      m << "# HELP edge_model_schema Schema label of current model\n";
      m << "# TYPE edge_model_schema gauge\n";
      m << "edge_model_schema{schema=\"" << schema << "\"} 1\n";
    }
    if(!mode.empty()){
      m << "# HELP edge_model_mode Mode label of current model\n";
      m << "# TYPE edge_model_mode gauge\n";
      m << "edge_model_mode{mode=\"" << mode << "\"} 1\n";
    }

    // --- Новые валидационные метрики (B) ---
    double val_acc = 0.0, val_rew = 0.0;
    long   val_size = 0, labeled = 0;

    if(model.is_object() && model.contains("metrics") && model["metrics"].is_object()){
      const json& mm = model["metrics"];
      // допускаем оба варианта ключей
      val_acc  = mm.value("val_accuracy", mm.value("val_acc", 0.0));
      val_rew  = mm.value("val_reward",   mm.value("val_rew", 0.0));
      val_size = mm.value("val_size", 0);
      labeled  = mm.value("M_labeled", 0);
    }

    m << "# HELP edge_val_accuracy Validation accuracy of latest model\n";
    m << "# TYPE edge_val_accuracy gauge\n";
    m << "edge_val_accuracy " << val_acc << "\n";

    m << "# HELP edge_val_reward Validation reward of latest model\n";
    m << "# TYPE edge_val_reward gauge\n";
    m << "edge_val_reward " << val_rew << "\n";

    m << "# HELP edge_val_size Validation sample size\n";
    m << "# TYPE edge_val_size gauge\n";
    m << "edge_val_size " << val_size << "\n";

    m << "# HELP edge_labeled_count Labeled training samples (confident)\n";
    m << "# TYPE edge_labeled_count gauge\n";
    m << "edge_labeled_count " << labeled << "\n";

    res.set_content(m.str(), "text/plain; version=0.0.4");
  });
}
