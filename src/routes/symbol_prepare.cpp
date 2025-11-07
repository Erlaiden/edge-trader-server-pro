#include "../httplib.h"
#include "../server_accessors.h"
#include "../utils_data.h"
#include "../train_logic.h"
#include "../utils.h"
#include "json.hpp"
#include <fstream>
#include <thread>
#include <chrono>
#include <cstdlib>

using json = nlohmann::json;

static bool cache_exists_and_fresh(const std::string& symbol, const std::string& interval, int min_rows = 5000) {
    std::string path = "cache/" + symbol + "_" + interval + ".csv";
    std::ifstream f(path);
    if (!f.good()) return false;
    int lines = 0;
    std::string line;
    while (std::getline(f, line)) ++lines;
    return lines >= min_rows;
}

// Проверка что модель ВАЛИДНАЯ (ok: true AND model_valid: true)
static bool model_valid(const std::string& symbol, const std::string& interval) {
    std::string path = "cache/models/" + symbol + "_" + interval + "_ppo_pro.json";
    std::ifstream f(path);
    if (!f.good()) return false;

    try {
        json model;
        f >> model;
        return model.value("ok", false) && model.value("model_valid", false);
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

        // КРИТИЧНО: проверяем ok: true AND model_valid: true
        if (!model.value("ok", false)) return false;
        if (!model.value("model_valid", false)) return false;

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

// Используем C++ API напрямую вместо curl
static bool fetch_and_aggregate(const std::string& symbol, int months = 6) {
    try {
        // 1. Скачиваем 15m за 6 месяцев через backfill_last_months
        json result = etai::backfill_last_months(symbol, "15", months, "linear");
        
        if (!result.value("ok", false)) {
            std::cerr << "[PREPARE] Failed to fetch 15m data: " 
                      << result.value("error", "unknown") << std::endl;
            return false;
        }

        int rows_15 = result.value("rows", 0);
        std::cout << "[PREPARE] Downloaded " << rows_15 << " rows of 15m data" << std::endl;

        if (rows_15 < 5000) {
            std::cerr << "[PREPARE] Insufficient 15m data: " << rows_15 << " rows" << std::endl;
            return false;
        }

        // 2. Агрегируем 15m -> 60/240/1440 через скрипт (проверенная логика)
        std::string cmd = "/opt/edge-trader-server/scripts/fetch_15m_and_agg.sh " + symbol + " " + std::to_string(months);
        std::cout << "[PREPARE] Running aggregation: " << cmd << std::endl;
        
        int ret = std::system(cmd.c_str());
        
        if (ret != 0) {
            std::cerr << "[PREPARE] Aggregation script failed with code: " << ret << std::endl;
            return false;
        }

        // 3. Проверяем что агрегация создала файлы
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        bool ok60   = cache_exists_and_fresh(symbol, "60",   1000);
        bool ok240  = cache_exists_and_fresh(symbol, "240",  500);
        bool ok1440 = cache_exists_and_fresh(symbol, "1440", 100);

        std::cout << "[PREPARE] Aggregation results: 60=" << ok60 
                  << " 240=" << ok240 << " 1440=" << ok1440 << std::endl;

        return ok60 && ok240 && ok1440;

    } catch (const std::exception& e) {
        std::cerr << "[PREPARE] Exception in fetch_and_aggregate: " << e.what() << std::endl;
        return false;
    }
}

void register_symbol_prepare_routes(httplib::Server& svr) {
    svr.Post("/api/symbol/prepare", [](const httplib::Request& req, httplib::Response& res) {
        json out;

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
        int months = body.value("months", 6); // FIXED: дефолт 6 месяцев

        if (symbol.empty()) {
            out["ok"] = false;
            out["error"] = "symbol_required";
            res.set_content(out.dump(2), "application/json");
            return;
        }

        out["symbol"] = symbol;
        out["interval"] = interval;
        out["months"] = months;
        json steps = json::array();

        // 1. Проверка кэшей (требуем минимум 5000 строк для 15m = ~52 дня)
        bool has_cache_15   = cache_exists_and_fresh(symbol, "15",   5000);
        bool has_cache_60   = cache_exists_and_fresh(symbol, "60",   1000);
        bool has_cache_240  = cache_exists_and_fresh(symbol, "240",  500);
        bool has_cache_1440 = cache_exists_and_fresh(symbol, "1440", 100);

        out["cache_status"] = {
            {"15",   has_cache_15},
            {"60",   has_cache_60},
            {"240",  has_cache_240},
            {"1440", has_cache_1440}
        };

        // 2. Скачивание и агрегация если нужно
        bool all_cached = has_cache_15 && has_cache_60 && has_cache_240 && has_cache_1440;
        
        if (!all_cached) {
            steps.push_back("fetch_and_aggregate");
            std::cout << "[PREPARE] Starting fetch and aggregation for " << symbol 
                      << " (" << months << " months)" << std::endl;
            
            if (!fetch_and_aggregate(symbol, months)) {
                out["ok"] = false;
                out["error"] = "fetch_and_aggregate_failed";
                out["steps"] = steps;
                res.set_content(out.dump(2), "application/json");
                return;
            }

            // Обновляем статус кэшей после агрегации
            has_cache_15   = cache_exists_and_fresh(symbol, "15",   5000);
            has_cache_60   = cache_exists_and_fresh(symbol, "60",   1000);
            has_cache_240  = cache_exists_and_fresh(symbol, "240",  500);
            has_cache_1440 = cache_exists_and_fresh(symbol, "1440", 100);

            out["cache_status"] = {
                {"15",   has_cache_15},
                {"60",   has_cache_60},
                {"240",  has_cache_240},
                {"1440", has_cache_1440}
            };
        } else {
            steps.push_back("cache_already_fresh");
        }

        // 3. Проверка модели (ВАЛИДНАЯ?)
        bool has_valid_model = model_valid(symbol, interval);
        out["model_valid"] = has_valid_model;

        // 4. Обучение если модель битая или отсутствует
        if (!has_valid_model) {
            steps.push_back("training");
            std::cout << "[PREPARE] Training model for " << symbol << std::endl;
            
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
                    out["train_details"] = train_result;
                    out["steps"] = steps;
                    res.set_content(out.dump(2), "application/json");
                    return;
                }

                if (!train_result.value("model_valid", false)) {
                    out["ok"] = false;
                    out["error"] = "model_invalid_after_training";
                    out["validation_error"] = train_result.value("validation_error", "unknown");
                    out["train_details"] = train_result;
                    out["steps"] = steps;
                    res.set_content(out.dump(2), "application/json");
                    return;
                }

                out["training"] = {
                    {"accuracy", train_result["metrics"].value("val_accuracy", 0.0)},
                    {"best_thr", train_result.value("best_thr", 0.0)},
                    {"M_labeled", train_result["metrics"].value("M_labeled", 0)},
                    {"feat_dim", train_result.value("feat_dim", 0)},
                    {"version", train_result.value("version", 0)}
                };

                has_valid_model = true;
            } catch (const std::exception& e) {
                out["ok"] = false;
                out["error"] = "training_exception";
                out["message"] = e.what();
                out["steps"] = steps;
                res.set_content(out.dump(2), "application/json");
                return;
            }
        } else {
            steps.push_back("model_already_valid");
        }

        // 5. Загрузка модели в RAM
        steps.push_back("loading_model");
        if (!load_model_to_ram(symbol, interval)) {
            out["ok"] = false;
            out["error"] = "model_load_failed";
            out["steps"] = steps;
            res.set_content(out.dump(2), "application/json");
            return;
        }

        // 6. Финальная проверка
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
            {"best_thr", model.value("best_thr", 0.0)},
            {"feat_dim", model.value("feat_dim", 0)},
            {"version", model.value("version", 0)}
        };

        std::cout << "[PREPARE] SUCCESS: " << symbol << " ready=" << ready << std::endl;
        res.set_content(out.dump(2), "application/json");
    });
}
