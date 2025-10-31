#include "httplib.h"
#include "../server_accessors.h"
#include "../rewardv2_accessors.h"
#include <sstream>
#include <iomanip>

namespace etai {

void register_metrics_routes(httplib::Server& srv){
    srv.Get("/metrics", [](const httplib::Request&, httplib::Response& res){
        std::ostringstream oss;
        oss.setf(std::ios::fixed); 
        oss.precision(12);

        // --- Model atoms ---
        oss << "# HELP edge_model_thr Model decision threshold\n";
        oss << "# TYPE edge_model_thr gauge\n";
        oss << "edge_model_thr " << etai::get_model_thr() << "\n";

        oss << "# HELP edge_model_ma_len Model moving-average length\n";
        oss << "# TYPE edge_model_ma_len gauge\n";
        oss << "edge_model_ma_len " << (long long)etai::get_model_ma_len() << "\n";

        oss << "# HELP edge_model_feat_dim Feature vector dimension of current policy\n";
        oss << "# TYPE edge_model_feat_dim gauge\n";
        oss << "edge_model_feat_dim " << (int)etai::get_model_feat_dim() << "\n";

        // --- Last inference telemetry ---
        oss << "# HELP edge_last_infer_score Last inference score (policy output)\n";
        oss << "# TYPE edge_last_infer_score gauge\n";
        oss << "edge_last_infer_score " << etai::get_last_infer_score() << "\n";

        oss << "# HELP edge_last_infer_sigma Last inference volatility sigma\n";
        oss << "# TYPE edge_last_infer_sigma gauge\n";
        oss << "edge_last_infer_sigma " << etai::get_last_infer_sigma() << "\n";

        oss << "# HELP edge_last_infer_signal Last inference signal (-1 short, 0 flat, 1 long)\n";
        oss << "# TYPE edge_last_infer_signal gauge\n";
        oss << "edge_last_infer_signal " << etai::get_last_infer_signal() << "\n";

        // --- Reward v2 telemetry ---
        oss << "# HELP edge_reward_avg Average Reward v2 on validation @best_thr\n";
        oss << "# TYPE edge_reward_avg gauge\n";
        oss << "edge_reward_avg " << etai::get_reward_avg() << "\n";

        oss << "# HELP edge_reward_wctx Context-weighted Reward v2 on validation @best_thr\n";
        oss << "# TYPE edge_reward_wctx gauge\n";
        oss << "edge_reward_wctx " << etai::get_reward_wctx() << "\n";

        oss << "# HELP edge_sharpe Sharpe ratio on validation @best_thr\n";
        oss << "# TYPE edge_sharpe gauge\n";
        oss << "edge_sharpe " << etai::get_reward_sharpe() << "\n";

        oss << "# HELP edge_winrate Win rate on validation @best_thr\n";
        oss << "# TYPE edge_winrate gauge\n";
        oss << "edge_winrate " << etai::get_reward_winrate() << "\n";

        oss << "# HELP edge_drawdown Max drawdown on validation equity @best_thr\n";
        oss << "# TYPE edge_drawdown gauge\n";
        oss << "edge_drawdown " << etai::get_reward_drawdown() << "\n";

        // --- Effective (dynamic) coefficients exported from trainer ---
        oss << "# HELP edge_lambda_risk_eff Effective lambda risk used in last train\n";
        oss << "# TYPE edge_lambda_risk_eff gauge\n";
        oss << "edge_lambda_risk_eff " << etai::get_lambda_risk_eff() << "\n";

        oss << "# HELP edge_mu_manip_eff Effective mu manip used in last train\n";
        oss << "# TYPE edge_mu_manip_eff gauge\n";
        oss << "edge_mu_manip_eff " << etai::get_mu_manip_eff() << "\n";

        // --- Optional anti-manip gauges (if trainer set them earlier) ---
        // Оставляем как есть: если атомики не выставлены — Prometheus всё равно съест нули.
        // Эти set_* могут не вызываться в текущей версии, но назад-совместимо.
        // Ключи названы так же, как ты уже использовал в истории.
        // (Если в коде нет сеттеров — будет просто 0.0)
        // edge_val_manip_ratio / edge_val_manip_flagged приходили из тренера ранее.
        // С текущими источниками они могут не обновляться — это ОК.
        // Сохраняем стабильность экспозиции.
        // oss << "edge_val_manip_ratio ...\n"; // сохраняем только те, что реально поддерживаем атомиками

        res.set_content(oss.str(), "text/plain; version=0.0.4");
    });
}

} // namespace etai
