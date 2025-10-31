#include "httplib.h"
#include "../server_accessors.h"
#include "../rewardv2_accessors.h"
#include <sstream>

namespace etai {

void register_metrics_routes(httplib::Server& srv) {
    srv.Get("/metrics", [](const httplib::Request&, httplib::Response& res) {
        std::ostringstream oss;
        oss.setf(std::ios::fixed);
        oss.precision(12);

        // Model info
        oss << "# HELP edge_model_thr Model decision threshold\n";
        oss << "# TYPE edge_model_thr gauge\n";
        oss << "edge_model_thr " << etai::get_model_thr() << "\n";

        oss << "# HELP edge_model_ma_len Model moving-average length\n";
        oss << "# TYPE edge_model_ma_len gauge\n";
        oss << "edge_model_ma_len " << (long long)etai::get_model_ma_len() << "\n";

        oss << "# HELP edge_model_feat_dim Feature vector dimension\n";
        oss << "# TYPE edge_model_feat_dim gauge\n";
        oss << "edge_model_feat_dim " << (int)etai::get_model_feat_dim() << "\n";

        // Last inference
        oss << "# HELP edge_last_infer_score Last inference score\n";
        oss << "# TYPE edge_last_infer_score gauge\n";
        oss << "edge_last_infer_score " << etai::get_last_infer_score() << "\n";

        oss << "# HELP edge_last_infer_sigma Last inference sigma\n";
        oss << "# TYPE edge_last_infer_sigma gauge\n";
        oss << "edge_last_infer_sigma " << etai::get_last_infer_sigma() << "\n";

        oss << "# HELP edge_last_infer_signal Last inference signal (-1,0,1)\n";
        oss << "# TYPE edge_last_infer_signal gauge\n";
        oss << "edge_last_infer_signal " << etai::get_last_infer_signal() << "\n";

        // Reward v2 metrics
        oss << "# HELP edge_reward_avg Average Reward v2\n";
        oss << "# TYPE edge_reward_avg gauge\n";
        oss << "edge_reward_avg " << etai::get_reward_avg() << "\n";

        oss << "# HELP edge_sharpe Sharpe ratio\n";
        oss << "# TYPE edge_sharpe gauge\n";
        oss << "edge_sharpe " << etai::get_reward_sharpe() << "\n";

        oss << "# HELP edge_winrate Win rate\n";
        oss << "# TYPE edge_winrate gauge\n";
        oss << "edge_winrate " << etai::get_reward_winrate() << "\n";

        oss << "# HELP edge_drawdown Max drawdown\n";
        oss << "# TYPE edge_drawdown gauge\n";
        oss << "edge_drawdown " << etai::get_reward_drawdown() << "\n";

        // Anti-manip telemetry
        oss << "# HELP edge_val_manip_ratio Normalized manip ratio on validation\n";
        oss << "# TYPE edge_val_manip_ratio gauge\n";
        oss << "edge_val_manip_ratio " << etai::get_val_manip_ratio() << "\n";

        oss << "# HELP edge_val_manip_flagged Count of manip flags on validation\n";
        oss << "# TYPE edge_val_manip_flagged gauge\n";
        oss << "edge_val_manip_flagged " << etai::get_val_manip_flagged() << "\n";

        res.set_content(oss.str(), "text/plain; version=0.0.4");
    });
}

} // namespace etai
