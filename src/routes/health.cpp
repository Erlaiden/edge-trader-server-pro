#include "httplib.h"
#include "health.h"
#include "../utils_data.h"
#include "../json.hpp"

using json = nlohmann::json;

void register_health_routes(httplib::Server& srv) {
    // --- /api/health ---
    srv.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
        json j;
        j["ok"] = true;
        res.set_content(j.dump(), "application/json");
    });

    // --- /api/health/ai ---
    srv.Get("/api/health/ai", [](const httplib::Request&, httplib::Response& res) {
        json j;
        // Возвращаем состояние данных как JSON
        j["ok"] = true;
        j["data"] = etai::get_data_health();
        res.set_content(j.dump(2), "application/json");
    });
}
