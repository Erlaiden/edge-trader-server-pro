#include "json.hpp"
#include "httplib.h"
#include "../robot/jwt_middleware.cpp"
#include "../robot/db_helper.cpp"

using json = nlohmann::json;

void register_trades_routes(httplib::Server& svr) {

    // GET /api/trades - получить историю сделок пользователя
    svr.Get("/api/trades", [&](const httplib::Request& req, httplib::Response& res){
        int user_id;
        if (!jwt_middleware::require_auth(req, res, user_id)) {
            return;
        }

        // Параметр limit (по умолчанию 50)
        int limit = 50;
        if (req.has_param("limit")) {
            try {
                limit = std::stoi(req.get_param_value("limit"));
                if (limit > 200) limit = 200;  // максимум 200
            } catch(...) {}
        }

        json trades = db::get_user_trades(user_id, limit);

        json out{
            {"ok", true},
            {"trades", trades},
            {"count", trades.size()}
        };

        res.set_content(out.dump(), "application/json");
    });

    // GET /api/journal - алиас для /api/trades (для совместимости)
    svr.Get("/api/journal", [&](const httplib::Request& req, httplib::Response& res){
        int user_id;
        if (!jwt_middleware::require_auth(req, res, user_id)) {
            return;
        }

        int limit = 50;
        if (req.has_param("limit")) {
            try {
                limit = std::stoi(req.get_param_value("limit"));
                if (limit > 200) limit = 200;
            } catch(...) {}
        }

        json trades = db::get_user_trades(user_id, limit);

        json out{
            {"ok", true},
            {"trades", trades},
            {"count", trades.size()}
        };

        res.set_content(out.dump(), "application/json");
    });
}
