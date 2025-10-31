#include "httplib.h"
#include "../server_accessors.h"
#include <sstream>

namespace etai {

void register_metrics_routes(httplib::Server& srv){
    srv.Get("/metrics", [](const httplib::Request&, httplib::Response& res){
        std::ostringstream oss;
        oss.setf(std::ios::fixed); oss.precision(12);

        // Модельные атомики
        oss << "# HELP edge_model_thr Model decision threshold\n";
        oss << "# TYPE edge_model_thr gauge\n";
        oss << "edge_model_thr " << etai::get_model_thr() << "\n";

        oss << "# HELP edge_model_ma_len Model moving-average length\n";
        oss << "# TYPE edge_model_ma_len gauge\n";
        oss << "edge_model_ma_len " << (long long)etai::get_model_ma_len() << "\n";

        oss << "# HELP edge_model_feat_dim Feature vector dimension of current policy\n";
        oss << "# TYPE edge_model_feat_dim gauge\n";
        oss << "edge_model_feat_dim " << (int)etai::get_model_feat_dim() << "\n";

        // Телеметрия последнего инференса
        oss << "# HELP edge_last_infer_score Last inference score (policy output)\n";
        oss << "# TYPE edge_last_infer_score gauge\n";
        oss << "edge_last_infer_score " << etai::get_last_infer_score() << "\n";

        oss << "# HELP edge_last_infer_sigma Last inference volatility sigma\n";
        oss << "# TYPE edge_last_infer_sigma gauge\n";
        oss << "edge_last_infer_sigma " << etai::get_last_infer_sigma() << "\n";

        oss << "# HELP edge_last_infer_signal Last inference signal (-1 short, 0 flat, 1 long)\n";
        oss << "# TYPE edge_last_infer_signal gauge\n";
        oss << "edge_last_infer_signal " << etai::get_last_infer_signal() << "\n";

        // Reward v2
        oss << "# HELP edge_reward_avg Average reward on validation @best_thr\n";
        oss << "# TYPE edge_reward_avg gauge\n";
        oss << "edge_reward_avg " << etai::get_reward_avg() << "\n";

        oss << "# HELP edge_sharpe Sharpe ratio on validation @best_thr\n";
        oss << "# TYPE edge_sharpe gauge\n";
        oss << "edge_sharpe " << etai::get_reward_sharpe() << "\n";

        oss << "# HELP edge_winrate Win rate on validation @best_thr\n";
        oss << "# TYPE edge_winrate gauge\n";
        oss << "edge_winrate " << etai::get_reward_winrate() << "\n";

        oss << "# HELP edge_drawdown Max drawdown on validation equity @best_thr\n";
        oss << "# TYPE edge_drawdown gauge\n";
        oss << "edge_drawdown " << etai::get_reward_drawdown() << "\n";

        res.set_content(oss.str(), "text/plain; version=0.0.4");
    });
}

} // namespace etai
