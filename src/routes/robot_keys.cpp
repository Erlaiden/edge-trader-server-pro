#include "../json.hpp"
#include <httplib.h>
#include "../robot/jwt_middleware.cpp"
#include "../robot/db_helper.cpp"

using json = nlohmann::json;

void register_robot_keys_routes(httplib::Server& srv) {
    
    // POST /api/robot/keys - Сохранить API ключи (per-user)
    srv.Post("/api/robot/keys", [&](const httplib::Request& req, httplib::Response& res){
        json out{{"ok", false}};
        
        int user_id;
        if (!jwt_middleware::require_auth(req, res, user_id)) {
            return; // 401 already set
        }
        
        try {
            json in = json::parse(req.body);
            
            std::string api_key = in.value("apiKey", "");
            std::string api_secret = in.value("apiSecret", "");
            bool testnet = in.value("testnet", false);
            
            if (api_key.empty() || api_secret.empty()) {
                out["error"] = "missing_keys";
                res.set_content(out.dump(), "application/json");
                return;
            }
            
            // Сохраняем в PostgreSQL для этого user_id
            if (!db::save_user_api_keys(user_id, api_key, api_secret, testnet)) {
                out["error"] = "database_error";
                res.set_content(out.dump(), "application/json");
                return;
            }
            
            out["ok"] = true;
            out["message"] = "keys_saved";
            res.set_content(out.dump(), "application/json");
            
        } catch(...) {
            out["error"] = "exception";
            res.set_content(out.dump(), "application/json");
        }
    });
    
    // GET /api/robot/keys - Получить API ключи (per-user)
    srv.Get("/api/robot/keys", [&](const httplib::Request& req, httplib::Response& res){
        json out{{"ok", false}};
        
        int user_id;
        if (!jwt_middleware::require_auth(req, res, user_id)) {
            return;
        }
        
        try {
            json keys = db::get_user_api_keys(user_id);
            
            if (keys.empty()) {
                out["ok"] = true;
                out["keys_present"] = false;
                res.set_content(out.dump(), "application/json");
                return;
            }
            
            out["ok"] = true;
            out["keys_present"] = true;
            out["testnet"] = keys.value("testnet", false);
            // НЕ возвращаем сами ключи по соображениям безопасности
            
            res.set_content(out.dump(), "application/json");
            
        } catch(...) {
            out["error"] = "exception";
            res.set_content(out.dump(), "application/json");
        }
    });
}
