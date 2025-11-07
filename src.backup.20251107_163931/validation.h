#pragma once
#include "json.hpp"
#include <string>

namespace etai {

// Результат валидации с детализацией
struct ValidationResult {
    bool ok;
    std::string level;  // "ok", "warning", "error", "critical"
    std::string message;
    nlohmann::json details;
    
    ValidationResult(bool ok_ = true, 
                    const std::string& level_ = "ok",
                    const std::string& msg_ = "",
                    const nlohmann::json& det_ = nlohmann::json::object())
        : ok(ok_), level(level_), message(msg_), details(det_) {}
};

// Агрегированный отчёт
struct HealthReport {
    bool ready;  // готов к inference?
    std::vector<ValidationResult> checks;
    nlohmann::json summary;
    
    nlohmann::json to_json() const;
};

// === Валидаторы ===

// 1. Проверка кэша данных для symbol/interval
ValidationResult validate_data_cache(const std::string& symbol, 
                                     const std::string& interval);

// 2. Проверка модели (структура, параметры, norm)
ValidationResult validate_model(const nlohmann::json& model,
                               const std::string& expected_symbol = "",
                               const std::string& expected_interval = "");

// 3. Проверка синхронизации (model ↔ data)
ValidationResult validate_sync(const std::string& symbol,
                              const std::string& interval,
                              const nlohmann::json& model);

// 4. Проверка HTF доступности
ValidationResult validate_htf(const std::string& symbol,
                             const std::vector<std::string>& intervals);

// 5. Проверка актуальности данных (timestamp)
ValidationResult validate_data_freshness(const std::string& symbol,
                                        const std::string& interval,
                                        long long max_age_ms);

// === Комплексная проверка ===
HealthReport check_inference_readiness(const std::string& symbol,
                                      const std::string& interval,
                                      const std::vector<std::string>& htf_intervals = {"60", "240", "1440"});

} // namespace etai
