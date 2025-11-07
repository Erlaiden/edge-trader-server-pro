#include <httplib.h>
#include <armadillo>
#include "json.hpp"
#include "server_accessors.h"

#include "routes/health.cpp"
#include "routes/health_ai.cpp"
#include "routes/train.cpp"
#include "routes/model.cpp"
#include "routes/infer.cpp"
#include "routes/metrics.cpp"
#include "routes/agents.cpp"
#include "routes/backfill.inc.cpp"
#include "routes/train_env.cpp"
#include "routes/model_set.cpp"
#include "routes/symbol_queue.cpp"
#include "routes/symbol.cpp"
#include "routes/diagnostic.cpp"
#include "routes/symbol_prepare.cpp"
#include "routes/pipeline.cpp"
#include "routes/compat_stubs.cpp"
#include "routes/version_status.cpp"

#include "robot/utils.cpp"
#include "robot/bybit_helpers.cpp"
#include "robot/signal_adapter.cpp"
#include "robot/robot_loop.cpp"
#include "routes/robot.cpp"

void register_robot_routes(httplib::Server& srv);

inline void enable_cors_and_errors(httplib::Server& svr){
    svr.set_error_handler([](const httplib::Request& req, httplib::Response& res){
        json j; j["ok"]=false; j["error"]="not_found"; j["path"]=req.path;
        res.status=404; res.set_content(j.dump(),"application/json");
    });
    svr.set_pre_routing_handler([](const httplib::Request&, httplib::Response& res){
        res.set_header("Access-Control-Allow-Origin","*");
        res.set_header("Access-Control-Allow-Methods","GET,POST,PUT,DELETE,OPTIONS");
        res.set_header("Access-Control-Allow-Headers","Content-Type,Authorization,X-BAPI-API-KEY,X-BAPI-SIGN,X-BAPI-TIMESTAMP,X-BAPI-RECV-WINDOW");
        return httplib::Server::HandlerResponse::Unhandled;
    });
}

int main(int argc, char** argv) {
    int port = 3000;
    if (argc > 1) port = std::atoi(argv[1]);
    
    etai::init_model_atoms_from_disk(
        "cache/models/BTCUSDT_15_ppo_pro.json",
        0.30, 12, 32
    );
    
    httplib::Server svr;
    enable_cors_and_errors(svr);
    
    register_health_routes(svr);
    register_health_ai(svr);
    register_train_routes(svr);
    register_model_routes(svr);
    register_model_set_routes(svr);
    register_infer_routes(svr);
    etai::register_metrics_routes(svr);
    register_backfill_routes(svr);
    etai::setup_agents_routes(svr);
    register_train_env_routes(svr);
    register_symbol_routes(svr);
    register_diagnostic_routes(svr);
    register_symbol_prepare_routes(svr);
    register_robot_routes(svr);
    register_pipeline_routes(svr);
    register_compat_stubs(svr);
    register_version_status_routes(svr);
    
    std::cout << "[EdgeTrader] Server started on port " << port << std::endl;
    std::cout << "[EdgeTrader] 🤖 Robot: READY" << std::endl;
    std::cout << "[EdgeTrader] Model: thr=" << etai::get_model_thr()
              << " ma=" << etai::get_model_ma_len()
              << " feat=" << etai::get_model_feat_dim() << std::endl;
    
    svr.listen("0.0.0.0", port);
    
    robot::stop();
    return 0;
}
