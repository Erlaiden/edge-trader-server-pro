#include "model.h"
#include "../model_lifecycle.h"
#include "../json.hpp"
#include "../http_helpers.h"
#include "../server_accessors.h"
#include "../utils_model.h"
#include <httplib.h>
#include <fstream>
#include <iostream>

using json = nlohmann::json;

void register_model_routes(httplib::Server& svr) {
    
    // GET /api/model - возвращает параметры загруженной модели
    svr.Get("/api/model", [](const httplib::Request& req, httplib::Response& res) {
        std::string symbol = qp(req, "symbol", "BTCUSDT");
        std::string interval = qp(req, "interval", "15");
        
        // Читаем модель с диска
        std::string model_path = "cache/models/" + symbol + "_" + interval + "_ppo_pro.json";
        std::ifstream mf(model_path);
        
        if (!mf) {
            json out{
                {"ok", false},
                {"error", "model_not_found"},
                {"symbol", symbol},
                {"interval", interval}
            };
            res.set_content(out.dump(), "application/json");
            return;
        }
        
        try {
            json model;
            mf >> model;
            
            json out{
                {"ok", true},
                {"symbol", symbol},
                {"interval", interval},
                {"version", model.value("version", 0)},
                {"feat_dim", model.value("feat_dim", 0)},
                {"tp", model.value("tp", 0.0)},
                {"sl", model.value("sl", 0.0)},
                {"best_thr", model.value("best_thr", 0.0)},
                {"ma_len", model.value("ma_len", 0)}
            };
            
            // ДОБАВЛЯЕМ ВОЗРАСТ МОДЕЛИ
            int age_days = etai::get_model_lifecycle().get_model_age_days(symbol, interval);
            if (age_days >= 0) {
                out["model_age_days"] = age_days;
                out["model_expires_in_days"] = 7 - age_days;
                out["model_needs_retrain"] = (age_days >= 7);
                
                if (age_days >= 6) {
                    out["warning"] = "Model is old, retrain recommended";
                } else if (age_days >= 5) {
                    out["info"] = "Model will expire soon";
                }
            }
            
            res.set_content(out.dump(), "application/json");
        } catch (const std::exception& e) {
            json out{
                {"ok", false},
                {"error", "model_parse_failed"},
                {"what", e.what()}
            };
            res.set_content(out.dump(), "application/json");
        }
    });
    
    // GET /api/model/reload - перезагрузка модели в память
    svr.Get("/api/model/reload", [](const httplib::Request& req, httplib::Response& res) {
        std::string symbol = qp(req, "symbol", "BTCUSDT");
        std::string interval = qp(req, "interval", "15");
        
        std::string model_path = "cache/models/" + symbol + "_" + interval + "_ppo_pro.json";
        std::ifstream mf(model_path);
        
        if (!mf) {
            json out{{"ok", false}, {"error", "model_not_found"}};
            res.set_content(out.dump(), "application/json");
            return;
        }
        
        try {
            json model;
            mf >> model;
            
            double thr = model.value("best_thr", 0.5);
            int ma = model.value("ma_len", 12);
            int feat = model.value("feat_dim", 32);
            
            etai::set_model_thr(thr);
            etai::set_model_ma_len(ma);
            etai::set_model_feat_dim(feat);
            
            json out{
                {"ok", true},
                {"reloaded", true},
                {"thr", thr},
                {"ma_len", ma},
                {"feat_dim", feat}
            };
            res.set_content(out.dump(), "application/json");
        } catch (const std::exception& e) {
            json out{{"ok", false}, {"error", "reload_failed"}};
            res.set_content(out.dump(), "application/json");
        }
    });
}
