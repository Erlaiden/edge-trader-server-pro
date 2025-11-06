#include <iostream>
#include <cstdlib>
#include <httplib.h>
#include <armadillo>
#include "json.hpp"
#include "server_accessors.h"

// Подключаем роуты единым TU
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
#include "routes/robot.cpp"
#include "routes/pipeline.cpp"
#include "routes/compat_stubs.cpp"
#include "routes/cors_and_errors.cpp"
#include "routes/version_status.cpp"

int main(int argc, char** argv) {
    int port = 3000;
    if (argc > 1) port = std::atoi(argv[1]);

    etai::init_model_atoms_from_disk(
        "cache/models/BTCUSDT_15_ppo_pro.json",
        0.30, 12, 32
    );

    httplib::Server svr;
    enable_cors_and_errors(svr);

    // ВАЖНО: Специфичные роуты регистрируем ПЕРВЫМИ!
    register_health_routes(svr);
    register_health_ai(svr);
    register_train_routes(svr);
    register_model_routes(svr);
    register_model_set_routes(svr);
    register_infer_routes(svr);              // ← ОСНОВНОЙ роут
    etai::register_metrics_routes(svr);
    register_backfill_routes(svr);
    etai::setup_agents_routes(svr);
    register_train_env_routes(svr);
    register_symbol_routes(svr);
    register_diagnostic_routes(svr);
    register_symbol_prepare_routes(svr);
    register_robot_routes(svr);
    register_pipeline_routes(svr);
    
    // Заглушки и fallback'и - В САМОМ КОНЦЕ!
    register_compat_stubs(svr);             // ← ПОСЛЕ всех реальных роутов
    register_version_status_routes(svr);

    std::cout << "[EdgeTrader] server started on port " << port
              << " (thr=" << etai::get_model_thr()
              << ", ma="  << etai::get_model_ma_len()
              << ", feat="<< etai::get_model_feat_dim() << ")\n";

    svr.listen("0.0.0.0", port);
    return 0;
}
