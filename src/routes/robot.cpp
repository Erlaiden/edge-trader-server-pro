#include "json.hpp"
#include "httplib.h"
#include "../robot/jwt_middleware.cpp"
#include "../robot/db_helper.cpp"

using json = nlohmann::json;

namespace robot {
    struct RobotConfig;
    extern bool start(const std::string& apiKey, const std::string& apiSecret, const RobotConfig& cfg, int user_id);
    extern void stop();
    extern bool is_running();
    extern RobotConfig get_config();
    extern double get_balance(const std::string& apiKey, const std::string& apiSecret);
    extern json get_position(const std::string& apiKey, const std::string& apiSecret, const std::string& symbol);
}

void register_robot_routes(httplib::Server& svr) {
    std::cout << "[DEBUG] register_robot_routes() called" << std::endl;

    // ТЕСТОВЫЙ ENDPOINT БЕЗ JWT
    svr.Get("/api/robot/test", [](const httplib::Request&, httplib::Response& res){
        json out{{"ok", true}, {"message", "test_works_no_jwt"}};
        res.set_content(out.dump(), "application/json");
    });

    // GET /api/robot/status - требует JWT
    svr.Get("/api/robot/status", [](const httplib::Request& req, httplib::Response& res){
        json out{{"ok", true}, {"running", false}, {"keys_present", false}};

        int user_id;
        if (!jwt_middleware::require_auth(req, res, user_id)) {
            return;
        }

        json keys = db::get_user_api_keys(user_id);
        out["keys_present"] = !keys.empty();
        out["running"] = robot::is_running();

        res.set_content(out.dump(), "application/json");
    });

    // GET /api/robot/balance - требует JWT
    svr.Get("/api/robot/balance", [](const httplib::Request& req, httplib::Response& res){
        json out{{"ok", false}, {"balance", 0}, {"available", 0}};

        int user_id;
        if (!jwt_middleware::require_auth(req, res, user_id)) {
            return;
        }

        try {
            json keys = db::get_user_api_keys(user_id);

            if (keys.empty()) {
                res.set_content(out.dump(), "application/json");
                return;
            }

            double balance = robot::get_balance(keys["apiKey"], keys["apiSecret"]);
            out["ok"] = true;
            out["balance"] = balance;
            out["available"] = balance;
            res.set_content(out.dump(), "application/json");

        } catch(...) {
            res.set_content(out.dump(), "application/json");
        }
    });

    // GET /api/robot/position - требует JWT
    svr.Get("/api/robot/position", [](const httplib::Request& req, httplib::Response& res){
        json out{{"ok", false}, {"position", nullptr}};

        int user_id;
        if (!jwt_middleware::require_auth(req, res, user_id)) {
            return;
        }

        try {
            json keys = db::get_user_api_keys(user_id);

            if (keys.empty()) {
                res.set_content(out.dump(), "application/json");
                return;
            }

            std::string symbol = req.get_param_value("symbol");
            if (symbol.empty()) symbol = "BTCUSDT";

            json pos = robot::get_position(keys["apiKey"], keys["apiSecret"], symbol);
            out["ok"] = true;
            out["position"] = pos;
            res.set_content(out.dump(), "application/json");
        } catch(...) {
            res.set_content(out.dump(), "application/json");
        }
    });

    // GET /api/robot/config - требует JWT
    svr.Get("/api/robot/config", [](const httplib::Request& req, httplib::Response& res){
        json out{{"ok", false}};

        int user_id;
        if (!jwt_middleware::require_auth(req, res, user_id)) {
            return;
        }

        json config = db::get_user_config(user_id);
        out["ok"] = true;
        out["config"] = config;
        res.set_content(out.dump(), "application/json");
    });

    // POST /api/robot/config - требует JWT
    svr.Post("/api/robot/config", [](const httplib::Request& req, httplib::Response& res){
        json out{{"ok", false}};

        int user_id;
        if (!jwt_middleware::require_auth(req, res, user_id)) {
            return;
        }

        try {
            json config = json::parse(req.body);
            bool success = db::save_user_config(user_id, config);

            out["ok"] = success;
            if (success) {
                std::cout << "[ROBOT_CONFIG] Saved for user_id=" << user_id << std::endl;
            }
        } catch (const std::exception& e) {
            out["error"] = e.what();
        }

        res.set_content(out.dump(), "application/json");
    });

    // GET /api/robot/pnl - требует JWT
    svr.Get("/api/robot/pnl", [](const httplib::Request& req, httplib::Response& res){
        std::cout << "[DEBUG] /api/robot/pnl called" << std::endl;

        int user_id;
        if (!jwt_middleware::require_auth(req, res, user_id)) {
            return;
        }

        json pnl = db::get_user_pnl(user_id);
        json out{
            {"ok", true},
            {"today", pnl.value("today", 0.0)},
            {"total", pnl.value("total", 0.0)},
            {"unrealized", pnl.value("unrealized", 0.0)}
        };

        res.set_content(out.dump(), "application/json");
    });

    // POST /api/robot/start - требует JWT
    svr.Post("/api/robot/start", [](const httplib::Request& req, httplib::Response& res){
        json out{{"ok", false}};
        std::cout << "[DEBUG] /api/robot/start called" << std::endl;

        int user_id;
        if (!jwt_middleware::require_auth(req, res, user_id)) {
            return;
        }

        try {
            json keys = db::get_user_api_keys(user_id);
            if (keys.empty()) {
                out["error"] = "keys_missing";
                res.set_content(out.dump(), "application/json");
                return;
            }

            json cfg_json = db::get_user_config(user_id);

            robot::RobotConfig cfg;
            cfg.symbol = cfg_json.value("symbol", "AIAUSDT");
            cfg.leverage = cfg_json.value("leverage", 10);
            cfg.balance_percent = cfg_json.value("balancePercent", 90.0);
            cfg.tp_percent = cfg_json.value("tpPercent", 2.0);
            cfg.sl_percent = cfg_json.value("slPercent", 1.0);
            // ВАЖНО: по умолчанию порог уверенности берём мягкий (25%), дальше его можно крутить из приложения
            cfg.min_confidence = cfg_json.value("minConfidence", 25.0);
            cfg.check_interval_sec = cfg_json.value("checkInterval", 60);
            cfg.auto_trade = cfg_json.value("autoTrade", false);

            bool success = robot::start(keys["apiKey"], keys["apiSecret"], cfg, user_id);
            out["ok"] = success;
            out["running"] = robot::is_running();
            res.set_content(out.dump(), "application/json");

        } catch(const std::exception& e) {
            out["error"] = e.what();
            res.set_content(out.dump(), "application/json");
        }
    });

    // POST /api/robot/stop - требует JWT
    svr.Post("/api/robot/stop", [](const httplib::Request& req, httplib::Response& res){
        int user_id;
        if (!jwt_middleware::require_auth(req, res, user_id)) {
            return;
        }

        robot::stop();
        json out{{"ok", true}, {"running", false}};
        res.set_content(out.dump(), "application/json");
    });

    std::cout << "[DEBUG] register_robot_routes() completed" << std::endl;
}
