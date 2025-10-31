#include "httplib.h"
#include "../rt_metrics.h"
#include "../server_accessors.h"
#include <sstream>
#include <iomanip>

namespace etai {

void register_metrics_routes(httplib::Server& srv){
    srv.Get("/metrics", [](const httplib::Request&, httplib::Response& res){
        REQ_METRICS.fetch_add(1, std::memory_order_relaxed);

        std::ostringstream out;
        out << std::setprecision(12) << std::fixed;

        // Process
        out << "# HELP edge_process_start_ms Process start time in ms since epoch\n";
        out << "# TYPE edge_process_start_ms gauge\n";
        out << "edge_process_start_ms " << (double)PROCESS_START_MS << "\n";

        // Inference counters & last ts
        out << "# HELP edge_last_infer_ts Last inference timestamp (ms since epoch)\n";
        out << "# TYPE edge_last_infer_ts gauge\n";
        out << "edge_last_infer_ts " << (double)LAST_INFER_TS.load(std::memory_order_relaxed) << "\n";

        out << "# HELP edge_infer_sig_long_total Infer LONG decisions count\n";
        out << "# TYPE edge_infer_sig_long_total counter\n";
        out << "edge_infer_sig_long_total " << (double)INFER_SIG_LONG.load(std::memory_order_relaxed) << "\n";

        out << "# HELP edge_infer_sig_short_total Infer SHORT decisions count\n";
        out << "# TYPE edge_infer_sig_short_total counter\n";
        out << "edge_infer_sig_short_total " << (double)INFER_SIG_SHORT.load(std::memory_order_relaxed) << "\n";

        out << "# HELP edge_infer_sig_neutral_total Infer NEUTRAL decisions count\n";
        out << "# TYPE edge_infer_sig_neutral_total counter\n";
        out << "edge_infer_sig_neutral_total " << (double)INFER_SIG_NEUTRAL.load(std::memory_order_relaxed) << "\n";

        // Last inference scalars
        out << "# HELP edge_last_infer_score Last inference score (policy output)\n";
        out << "# TYPE edge_last_infer_score gauge\n";
        out << "edge_last_infer_score " << LAST_INFER_SCORE_AT.load(std::memory_order_relaxed) << "\n";

        out << "# HELP edge_last_infer_sigma Last inference volatility sigma\n";
        out << "# TYPE edge_last_infer_sigma gauge\n";
        out << "edge_last_infer_sigma " << LAST_INFER_SIGMA_AT.load(std::memory_order_relaxed) << "\n";

        out << "# HELP edge_last_infer_signal Last inference signal (-1 short, 0 flat, 1 long)\n";
        out << "# TYPE edge_last_infer_signal gauge\n";
        out << "edge_last_infer_signal " << LAST_INFER_SIGNAL_AT.load(std::memory_order_relaxed) << "\n";

        // Current model atoms (источник истины — server_accessors)
        out << "# HELP edge_model_thr Model decision threshold\n";
        out << "# TYPE edge_model_thr gauge\n";
        out << "edge_model_thr " << etai::get_model_thr() << "\n";

        out << "# HELP edge_model_ma_len Model moving-average length\n";
        out << "# TYPE edge_model_ma_len gauge\n";
        out << "edge_model_ma_len " << (double)etai::get_model_ma_len() << "\n";

        out << "# HELP edge_model_feat_dim Feature vector dimension of current policy\n";
        out << "# TYPE edge_model_feat_dim gauge\n";
        out << "edge_model_feat_dim " << (double)etai::get_model_feat_dim() << "\n";

        // Training aggregates (могут быть 0 до первого train)
        out << "# HELP edge_model_best_thr Best threshold from last training\n";
        out << "# TYPE edge_model_best_thr gauge\n";
        out << "edge_model_best_thr " << MODEL_BEST_THR.load(std::memory_order_relaxed) << "\n";

        res.set_content(out.str(), "text/plain; version=0.0.4");
    });
}

} // namespace etai
