// ===== Edge Trader Server (PRO-only): main.cpp =====
#include "httplib.h"
#include "json.hpp"

#include "rt_metrics.h"
#include "routes/health.h"
#include "routes/infer.h"
#include "routes/agents.h"
#include "routes/model.h"
#include "routes/metrics.h"
#include "routes/train.h"      // <== добавлено
#include "routes/backfill.inc.cpp" // <== чтобы регистрировать /api/backfill

#include "utils.h"
#include "fetch.h"
#include "ppo.h"
#include "ppo_pro.h"
#include "server_accessors.h"

#include <ctime>
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    int port = 3000;
    if (argc > 1) port = std::atoi(argv[1]);

    httplib::Server srv;

    // --- базовые маршруты ---
    register_health_routes(srv);
    register_infer_routes(srv);
    register_agents_routes(srv);
    register_model_routes(srv);
    register_metrics_routes(srv);
    register_backfill_routes(srv);
    register_train_routes(srv); // <== восстановлен train API

    // --- инициализация атомов модели ---
    etai::init_model_atoms_from_disk();

    std::printf("[SERVER] Edge Trader Server PRO-only running on :%d\n", port);
    srv.listen("0.0.0.0", port);
    return 0;
}
