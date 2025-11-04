#include "../httplib.h"
#include "../server_accessors.h"
#include "../utils_data.h"
#include "json.hpp"
#include <fstream>

using json = nlohmann::json;

static json check_cache_file(const std::string& symbol, const std::string& interval) {
    json r;
    std::string path = "cache/" + symbol + "_" + interval + ".csv";
    std::ifstream f(path);
    
    r["path"] = path;
    r["exists"] = f.good();
    
    if (f.good()) {
        int lines = 0;
        std::string line;
        while (std::getline(f, line)) ++lines;
        r["rows"] = lines - 1; // минус header
    } else {
        r["rows"] = 0;
    }
    
    return r;
}

void register_diagnostic_routes(httplib::Server& svr) {
    svr.Get("/api/diagnostic", [](const httplib::Request& req, httplib::Response& res) {
        json out;
        out["timestamp"] = (long long)time(nullptr) * 1000;
        
        // 1. Текущая модель из RAM
        json model = etai::get_current_model();
        std::string model_symbol   = model.value("symbol", "");
        std::string model_interval = model.value("interval", "");
        double model_thr      = model.value("best_thr", 0.0);
        int model_feat_dim    = 0;
        
        if (model.contains("policy") && model["policy"].contains("feat_dim"))
            model_feat_dim = model["policy"]["feat_dim"].get<int>();
        
        bool has_norm = false;
        if (model.contains("policy") && model["policy"].contains("norm"))
            has_norm = model["policy"]["norm"].is_object();
        
        json model_ram = json::object();
        model_ram["symbol"] = model_symbol.empty() ? json(nullptr) : json(model_symbol);
        model_ram["interval"] = model_interval.empty() ? json(nullptr) : json(model_interval);
        model_ram["best_thr"] = (model_thr > 0) ? json(model_thr) : json(nullptr);
        model_ram["feat_dim"] = (model_feat_dim > 0) ? json(model_feat_dim) : json(nullptr);
        model_ram["has_norm"] = has_norm;
        
        out["model_ram"] = model_ram;
        
        // 2. Атомики
        out["atomics"] = {
            {"thr", etai::get_model_thr()},
            {"ma_len", (long long)etai::get_model_ma_len()},
            {"feat_dim", etai::get_model_feat_dim()}
        };
        
        // 3. Проверка кэшей для модели
        json cache_checks = json::object();
        if (!model_symbol.empty() && !model_interval.empty()) {
            cache_checks["15"] = check_cache_file(model_symbol, model_interval);
            cache_checks["60"] = check_cache_file(model_symbol, "60");
            cache_checks["240"] = check_cache_file(model_symbol, "240");
            cache_checks["1440"] = check_cache_file(model_symbol, "1440");
        }
        out["cache"] = cache_checks;
        
        // 4. ПРОБЛЕМЫ (автоматический поиск)
        json issues = json::array();
        
        if (model_symbol.empty())
            issues.push_back("model_symbol_unknown");
        if (model_interval.empty())
            issues.push_back("model_interval_unknown");
        if (model_thr <= 0)
            issues.push_back("model_thr_invalid");
        if (model_feat_dim <= 0)
            issues.push_back("model_feat_dim_unknown");
        if (!has_norm)
            issues.push_back("model_missing_norm - CRITICAL: inference будет использовать локальный zscore!");
        
        // Проверка кэшей
        for (auto& kv : cache_checks.items()) {
            if (!kv.value()["exists"].get<bool>())
                issues.push_back("cache_missing_" + kv.key());
            else if (kv.value()["rows"].get<int>() < 100)
                issues.push_back("cache_too_small_" + kv.key());
        }
        
        // Несоответствия атомиков и модели
        if (model_thr > 0 && std::abs(model_thr - etai::get_model_thr()) > 1e-6)
            issues.push_back("atomic_thr_mismatch");
        if (model_feat_dim > 0 && model_feat_dim != etai::get_model_feat_dim())
            issues.push_back("atomic_feat_dim_mismatch");
        
        out["issues"] = issues;
        out["issues_count"] = (int)issues.size();
        out["status"] = issues.empty() ? "OK" : "HAS_ISSUES";
        
        res.set_content(out.dump(2), "application/json");
    });
}
