#include "../httplib.h"
#include "../server_accessors.h"
#include "../utils_data.h"
#include "../train_logic.h"
#include "../utils.h"
#include "json.hpp"
#include <fstream>
#include <thread>
#include <chrono>
#include <map>
#include <mutex>

using json = nlohmann::json;

// === АСИНХРОННОЕ ОБУЧЕНИЕ ===
static std::map<std::string, std::string> g_training_status;
static std::mutex g_training_mutex;

static bool cache_exists_and_fresh(const std::string& symbol, const std::string& interval, int min_rows = 1000) {
    std::string path = "cache/" + symbol + "_" + interval + ".csv";
    std::ifstream f(path);
    if (!f.good()) return false;
    int lines = 0;
    std::string line;
    while (std::getline(f, line)) ++lines;
    return lines >= min_rows;
}

static bool model_valid(const std::string& symbol, const std::string& interval) {
    std::string path = "cache/models/" + symbol + "_" + interval + "_ppo_pro.json";
    std::ifstream f(path);
    if (!f.good()) return false;

    try {
        json model;
        f >> model;
        return model.value("ok", false);
    } catch (...) {
        return false;
    }
}

static bool load_model_to_ram(const std::string& symbol, const std::string& interval) {
    const std::string path = "cache/models/" + symbol + "_" + interval + "_ppo_pro.json";
    std::ifstream f(path);
    if (!f.good()) return false;

    json model;
    try {
        f >> model;
        if (!model.value("ok", false)) return false;

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

static bool run_backfill(const std::string& symbol, const std::string& interval, int months = 6) {
    try {
        json result = etai::backfill_last_months(symbol, interval, months, "linear");
        if (!result.value("ok", false)) {
            std::cerr << "[PREPARE] Backfill failed for " << symbol << " " << interval
                      << ": " << result.value("error", "unknown") << std::endl;
            return false;
        }
        int rows = result.value("rows", 0);
        std::cout << "[PREPARE] Downloaded " << rows << " rows for "
                  << symbol << " " << interval << std::endl;
        return rows > 100;
    } catch (const std::exception& e) {
        std::cerr << "[PREPARE] Exception in backfill: " << e.what() << std::endl;
        return false;
    }
}

void register_symbol_prepare_routes(httplib::Server& svr) {
    
    // GET /api/symbol/status - проверка статуса обучения
    svr.Get("/api/symbol/status", [](const httplib::Request& req, httplib::Response& res) {
        std::string symbol = req.get_param_value("symbol");
        
        std::lock_guard<std::mutex> lock(g_training_mutex);
        std::string status = g_training_status[symbol];
        if (status.empty()) status = "idle";
        
        json out = {
            {"ok", true},
            {"symbol", symbol},
            {"status", status}
        };
        res.set_content(out.dump(), "application/json");
    });
    
    // POST /api/symbol/prepare - АСИНХРОННОЕ обучение
    svr.Post("/api/symbol/prepare", [](const httplib::Request& req, httplib::Response& res) {
        json body;
        try {
            body = json::parse(req.body);
        } catch (...) {
            json out = {{"ok", false}, {"error", "invalid_json"}};
            res.set_content(out.dump(2), "application/json");
            return;
        }

        std::string symbol = body.value("symbol", "");
        std::string interval = body.value("interval", "15");
        int months = body.value("months", 6);

        if (symbol.empty()) {
            json out = {{"ok", false}, {"error", "symbol_required"}};
            res.set_content(out.dump(2), "application/json");
            return;
        }

        // ✅ СРАЗУ ОТВЕЧАЕМ КЛИЕНТУ - не блокируем
        {
            std::lock_guard<std::mutex> lock(g_training_mutex);
            g_training_status[symbol] = "training";
        }
        
        json out = {
            {"ok", true},
            {"status", "training_started"},
            {"symbol", symbol},
            {"message", "Training started in background. Check /api/symbol/status?symbol=" + symbol}
        };
        res.set_content(out.dump(), "application/json");

        // 🚀 ВСЁ ОБУЧЕНИЕ В ФОНОВОМ ПОТОКЕ
        std::thread([symbol, interval, months]() {
            try {
                std::cout << "[ASYNC] Training started: " << symbol << std::endl;
                
                // 1. Backfill данных
                bool has_cache_15 = cache_exists_and_fresh(symbol, interval, 15000);
                if (!has_cache_15) {
                    std::cout << "[ASYNC] Backfilling 15m: " << symbol << " (" << months << " months)" << std::endl;
                    if (!run_backfill(symbol, interval, months)) {
                        std::lock_guard<std::mutex> lock(g_training_mutex);
                        g_training_status[symbol] = "error:backfill_failed";
                        std::cerr << "[ASYNC] ❌ Backfill failed: " << symbol << std::endl;
                        return;
                    }
                }
                
                // Backfill HTF
                bool has_cache_60 = cache_exists_and_fresh(symbol, "60", 3500);
                bool has_cache_240 = cache_exists_and_fresh(symbol, "240", 800);
                bool has_cache_1440 = cache_exists_and_fresh(symbol, "1440", 150);
                
                if (!has_cache_60) run_backfill(symbol, "60", months);
                if (!has_cache_240) run_backfill(symbol, "240", months);
                if (!has_cache_1440) run_backfill(symbol, "1440", months);

                // 2. Обучение модели
                bool has_valid_model = model_valid(symbol, interval);
                if (!has_valid_model) {
                    std::cout << "[ASYNC] Training model: " << symbol << std::endl;
                    
                    double tp = 0.02;
                    double sl = 0.01;
                    int ma_len = 12;
                    
                    json train_result = etai::run_train_pro_and_save(
                        symbol, interval, 10000, tp, sl, ma_len, false
                    );

                    if (!train_result.value("ok", false)) {
                        std::lock_guard<std::mutex> lock(g_training_mutex);
                        g_training_status[symbol] = "error:training_failed";
                        std::cerr << "[ASYNC] ❌ Training failed: " << symbol << std::endl;
                        return;
                    }
                    
                    std::cout << "[ASYNC] Training completed: " << symbol << std::endl;
                }

                // 3. Загрузка в RAM
                std::cout << "[ASYNC] Loading model to RAM: " << symbol << std::endl;
                if (!load_model_to_ram(symbol, interval)) {
                    std::lock_guard<std::mutex> lock(g_training_mutex);
                    g_training_status[symbol] = "error:load_failed";
                    std::cerr << "[ASYNC] ❌ Load failed: " << symbol << std::endl;
                    return;
                }

                // ✅ ГОТОВО
                {
                    std::lock_guard<std::mutex> lock(g_training_mutex);
                    g_training_status[symbol] = "ready";
                }
                std::cout << "[ASYNC] ✅ Completed: " << symbol << std::endl;
                
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(g_training_mutex);
                g_training_status[symbol] = std::string("error:") + e.what();
                std::cerr << "[ASYNC] ❌ Exception: " << e.what() << std::endl;
            }
        }).detach();
    });
}
