#include "../httplib.h"
#include "../server_accessors.h"
#include "../utils_data.h"
#include "../train_logic.h"
#include "json.hpp"
#include <fstream>
#include <thread>
#include <chrono>

using json = nlohmann::json;

// Проверка наличия и свежести кэша
static bool cache_exists_and_fresh(const std::string& symbol, const std::string& interval) {
    std::string path = "cache/" + symbol + "_" + interval + ".csv";
    std::ifstream f(path);
    if (!f.good()) return false;
    
    int lines = 0;
    std::string line;
    while (std::getline(f, line)) ++lines;
    
    return lines > 100; // минимум 100 строк
}

// Проверка наличия модели
static bool model_exists(const std::string& symbol, const std::string& interval) {
    std::string path = "cache/models/" + symbol + "_" + interval + "_ppo_pro.json";
    std::ifstream f(path);
    return f.good();
}

// Загрузка модели в RAM
static bool load_model_to_ram(const std::string& symbol, const std::string& interval) {
    const std::string path = "cache/models/" + symbol + "_" + interval + "_ppo_pro.json";
    std::ifstream f(path);
    if (!f.good()) return false;
    
    json model;
    try {
        f >> model;
        
        double best_thr = model.value("best_thr", 0.5);
        int ma_len = model.value("ma_len", 12);
        int feat_dim = 0;
        
        if (model.contains("policy") && model["policy"].contains("feat_dim"))
            feat_dim = model["policy"]["feat_dim"].get<int>();
        
        etai::set_model_thr(best_thr);
        etai::set_model_ma_len(ma_len);
        if (feat_dim > 0) etai::set_model_feat_dim(feat_dim);
        etai::set_current_model(model);
        
        return true;
    } catch (...) {
        return false;
    }
}

// Вызов backfill через system call (простой способ)
static bool run_backfill(const std::string& symbol, const std::string& interval) {
    std::string cmd = "curl -sS 'http://127.0.0.1:3000/api/backfill?symbol=" 
                    + symbol + "&interval=" + interval + "' > /dev/null 2>&1";
    int ret = system(cmd.c_str());
    std::this_thread::sleep_for(std::chrono::seconds(2)); // ждём завершения
    return ret == 0;
}

void register_symbol_prepare_routes(httplib::Server& svr) {
    svr.Post("/api/symbol/prepare", [](const httplib::Request& req, httplib::Response& res) {
        json out;
        
        // 1. Парсинг body
        json body;
        try {
            body = json::parse(req.body);
        } catch (...) {
            out["ok"] = false;
            out["error"] = "invalid_json";
            res.set_content(out.dump(2), "application/json");
            return;
        }
        
        std::string symbol = body.value("symbol", "");
        std::string interval = body.value("interval", "15");
        
        if (symbol.empty()) {
            out["ok"] = false;
            out["error"] = "symbol_required";
            res.set_content(out.dump(2), "application/json");
            return;
        }
        
        out["symbol"] = symbol;
        out["interval"] = interval;
        
        json steps = json::array();
        
        // 2. Проверка кэшей
        bool has_cache_15 = cache_exists_and_fresh(symbol, interval);
        bool has_cache_60 = cache_exists_and_fresh(symbol, "60");
        bool has_cache_240 = cache_exists_and_fresh(symbol, "240");
        bool has_cache_1440 = cache_exists_and_fresh(symbol, "1440");
        
        out["cache_status"] = {
            {"15", has_cache_15},
            {"60", has_cache_60},
            {"240", has_cache_240},
            {"1440", has_cache_1440}
        };
        
        // 3. Backfill если нужно
        if (!has_cache_15) {
            steps.push_back("backfill_15");
            if (!run_backfill(symbol, interval)) {
                out["ok"] = false;
                out["error"] = "backfill_failed_15";
                out["steps"] = steps;
                res.set_content(out.dump(2), "application/json");
                return;
            }
            has_cache_15 = true;
        }
        
        if (!has_cache_60) {
            steps.push_back("backfill_60");
            run_backfill(symbol, "60"); // optional
        }
        if (!has_cache_240) {
            steps.push_back("backfill_240");
            run_backfill(symbol, "240"); // optional
        }
        if (!has_cache_1440) {
            steps.push_back("backfill_1440");
            run_backfill(symbol, "1440"); // optional
        }
        
        // 4. Проверка модели
        bool has_model = model_exists(symbol, interval);
        out["model_exists"] = has_model;
        
        // 5. Обучение если нужно
        if (!has_model) {
            steps.push_back("training");
            try {
                double tp = 0.008;
                double sl = 0.004;
                int ma_len = 12;
                
                json train_result = etai::run_train_pro_and_save(
                    symbol, interval, 10000, tp, sl, ma_len, false
                );
                
                if (!train_result.value("ok", false)) {
                    out["ok"] = false;
                    out["error"] = "training_failed";
                    out["train_error"] = train_result;
                    out["steps"] = steps;
                    res.set_content(out.dump(2), "application/json");
                    return;
                }
                
                out["training"] = {
                    {"accuracy", train_result["metrics"].value("val_accuracy", 0.0)},
                    {"best_thr", train_result.value("best_thr", 0.0)}
                };
                
                has_model = true;
            } catch (const std::exception& e) {
                out["ok"] = false;
                out["error"] = "training_exception";
                out["message"] = e.what();
                out["steps"] = steps;
                res.set_content(out.dump(2), "application/json");
                return;
            }
        } else {
            steps.push_back("model_already_exists");
        }
        
        // 6. Загрузка модели в RAM
        steps.push_back("loading_model");
        if (!load_model_to_ram(symbol, interval)) {
            out["ok"] = false;
            out["error"] = "model_load_failed";
            out["steps"] = steps;
            res.set_content(out.dump(2), "application/json");
            return;
        }
        
        // 7. Финальная проверка готовности
        json model = etai::get_current_model();
        std::string loaded_sym = model.value("symbol", "");
        std::string loaded_int = model.value("interval", "");
        
        bool ready = (loaded_sym == symbol && loaded_int == interval);
        
        out["ok"] = true;
        out["ready"] = ready;
        out["steps"] = steps;
        out["loaded_model"] = {
            {"symbol", loaded_sym},
            {"interval", loaded_int},
            {"best_thr", model.value("best_thr", 0.0)}
        };
        
        res.set_content(out.dump(2), "application/json");
    });
}
