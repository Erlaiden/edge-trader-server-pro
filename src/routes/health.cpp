#include "httplib.h"
#include "health.h"
#include "../utils_data.h"
#include "../json.hpp"

using json = nlohmann::json;

void register_health_routes(httplib::Server& srv) {
    // --- /api/health --- простая проверка живости
    srv.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
        json j;
        j["ok"] = true;
        res.set_content(j.dump(), "application/json");
    });

    // ВНИМАНИЕ: /api/health/ai регистрируется в routes/health_ai.cpp
}
