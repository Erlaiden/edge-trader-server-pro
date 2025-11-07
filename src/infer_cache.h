#pragma once
#include "json.hpp"
#include <armadillo>
#include <string>
#include <mutex>
#include <chrono>
#include <map>

namespace etai {

struct CachedInferData {
    nlohmann::json model;
    arma::mat M15;
    arma::mat M60;
    arma::mat M240;
    arma::mat M1440;
    std::chrono::steady_clock::time_point loaded_at;
    
    bool is_valid() const {
        auto now = std::chrono::steady_clock::now();
        auto age = std::chrono::duration_cast<std::chrono::seconds>(now - loaded_at);
        return age.count() < 300; // 5 минут
    }
};

class InferCache {
private:
    std::mutex mtx;
    std::map<std::string, CachedInferData> cache;
    
public:
    bool get(const std::string& symbol, const std::string& interval, CachedInferData& out) {
        std::lock_guard<std::mutex> lock(mtx);
        std::string key = symbol + "_" + interval;
        
        auto it = cache.find(key);
        if (it == cache.end()) return false;
        if (!it->second.is_valid()) {
            cache.erase(it);
            return false;
        }
        
        out = it->second;
        return true;
    }
    
    void put(const std::string& symbol, const std::string& interval, const CachedInferData& data) {
        std::lock_guard<std::mutex> lock(mtx);
        std::string key = symbol + "_" + interval;
        cache[key] = data;
    }
    
    void invalidate(const std::string& symbol, const std::string& interval) {
        std::lock_guard<std::mutex> lock(mtx);
        std::string key = symbol + "_" + interval;
        cache.erase(key);
    }
    
    void clear() {
        std::lock_guard<std::mutex> lock(mtx);
        cache.clear();
    }
};

// Глобальный кэш
inline InferCache& get_infer_cache() {
    static InferCache cache;
    return cache;
}

} // namespace etai
