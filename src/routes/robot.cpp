#include "json.hpp"
#include <httplib.h>
#include "../robot/jwt_middleware.cpp"
#include "../robot/db_helper.cpp"

using json = nlohmann::json;

namespace robot {
    struct RobotConfig;
    extern bool start(const std::string& apiKey, const std::string& apiSecret, const RobotConfig& cfg);
    extern void stop();
    extern bool is_running();
    extern RobotConfig get_config();
    extern double get_balance(const std::string& apiKey, const std::string& apiSecret);
    extern json get_position(const std::string& apiKey, const std::string& apiSecret, const std::string& symbol);
}

void register_robot_routes(httplib::Server& srv) {
    
    // GET /api/robot/status - public (без auth)
    srv.Get("/api/robot/status", [&](const httplib::Request& req, httplib::Response& res){
        json out{{"ok", true}, {"running", robot::is_running()}};
        res.set_content(out.dump(), "application/json");
    });
    
    // POST /api/robot/keys - требует JWT
    srv.Post("/api/robot/keys", [&](const httplib::Request& req, httplib::Response& res){
        json out{{"ok", false}};
        
        int user_id;
        if (!jwt_middleware::require_auth(req, res, user_id)) {
            return;
        }
        
        try {
            json in = json::parse(req.body);
            std::string apiKey = in.value("apiKey", "");
            std::string apiSecret = in.value("apiSecret", "");
            bool testnet = in.value("testnet", false);
            
            if (apiKey.empty() || apiSecret.empty()) {
                out["error"] = "missing_keys";
                res.set_content(out.dump(), "application/json");
                return;
            }
            
            if (!db::save_user_api_keys(user_id, apiKey, apiSecret, testnet)) {
                out["error"] = "database_error";
                res.set_content(out.dump(), "application/json");
                return;
            }
            
            out["ok"] = true;
            res.set_content(out.dump(), "application/json");
            
        } catch(...) {
            out["error"] = "exception";
            res.set_content(out.dump(), "application/json");
        }
    });
    
    // GET /api/robot/balance - требует JWT
    srv.Get("/api/robot/balance", [&](const httplib::Request& req, httplib::Response& res){
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
    srv.Get("/api/robot/position", [&](const httplib::Request& req, httplib::Response& res){
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
    
    // GET /api/robot/pnl - требует JWT
    srv.Get("/api/robot/pnl", [&](const httplib::Request& req, httplib::Response& res){
        json out{{"ok", true}, {"today", 0}, {"total", 0}, {"unrealized", 0}};
        
        int user_id;
        if (!jwt_middleware::require_auth(req, res, user_id)) {
            return;
        }
        
        // TODO: implement real PnL calculation per user
        res.set_content(out.dump(), "application/json");
    });
}
