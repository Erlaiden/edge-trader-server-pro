#include <iostream>
#include <cstdlib>
#include <httplib.h>
#include <armadillo>
#include "json.hpp"

#include "server_accessors.h"   // etai::init_model_atoms_from_disk, getters

// Подключаем роуты единым TU
#include "routes/health.cpp"
#include "routes/health_ai.cpp"
#include "routes/train.cpp"
#include "routes/model.cpp"
#include "routes/infer.cpp"
#include "routes/metrics.cpp"     // внутри namespace etai
#include "routes/agents.cpp"
#include "routes/backfill.inc.cpp" // static inline register_backfill_routes(...)

int main(int argc, char** argv) {
    int port = 3000;
    if (argc > 1) port = std::atoi(argv[1]);

    // 1) Инициализация состояния модели (из диска + дефолты)
    etai::init_model_atoms_from_disk(
        "cache/models/BTCUSDT_15_ppo_pro.json",
        /*def_thr*/ 0.38,
        /*def_ma*/  12,
        /*feat*/    28
    );

    // 2) HTTP-сервер
    httplib::Server svr;

    // 3) Регистрация роутов
    register_health_routes(svr);
    register_health_ai(svr);
    register_train_routes(svr);
    register_model_routes(svr);
    register_infer_routes(svr);
    etai::register_metrics_routes(svr); // <-- квалификация namespace
    register_backfill_routes(svr);
    etai::setup_agents_routes(svr);

    std::cout << "[EdgeTrader] server started on port " << port
              << " (thr=" << etai::get_model_thr()
              << ", ma=" << etai::get_model_ma_len()
              << ", feat=" << etai::get_model_feat_dim() << ")\n";

    svr.listen("0.0.0.0", port);
    return 0;
}
