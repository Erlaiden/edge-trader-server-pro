#pragma once
#include <thread>
#include <atomic>
#include <chrono>
#include <iostream>
#include <vector>
#include <httplib.h>

namespace etai {

class AutoBackfill {
private:
    std::atomic<bool> running{false};
    std::thread backfill_thread;
    std::vector<std::string> symbols = {"BTCUSDT", "ETHUSDT", "SOLUSDT", "BNBUSDT"};
    int interval_minutes = 15;
    
    void backfill_loop() {
        std::cout << "[AUTO_BACKFILL] Started (every " << interval_minutes << " min)" << std::endl;
        
        while (running.load()) {
            auto now = std::chrono::system_clock::now();
            auto time_t = std::chrono::system_clock::to_time_t(now);
            auto tm = *std::gmtime(&time_t);
            
            // Обновляем только в начале каждого 15-минутного интервала
            if (tm.tm_min % interval_minutes == 0 && tm.tm_sec < 30) {
                std::cout << "[AUTO_BACKFILL] Updating data..." << std::endl;
                
                for (const auto& symbol : symbols) {
                    try {
                        // Вызываем внутренний backfill через HTTP
                        httplib::Client cli("localhost", 3000);
                        cli.set_connection_timeout(10);
                        
                        std::string path = "/api/backfill?symbol=" + symbol + "&interval=15&months=6";
                        auto res = cli.Get(path.c_str());
                        
                        if (res && res->status == 200) {
                            std::cout << "[AUTO_BACKFILL] ✅ " << symbol << " updated" << std::endl;
                        } else {
                            std::cerr << "[AUTO_BACKFILL] ❌ " << symbol << " failed" << std::endl;
                        }
                    } catch (const std::exception& e) {
                        std::cerr << "[AUTO_BACKFILL] Error for " << symbol << ": " << e.what() << std::endl;
                    }
                }
                
                // Очищаем кэш после обновления данных
                try {
                    httplib::Client cli("localhost", 3000);
                    cli.set_connection_timeout(5);
                    auto res = cli.Post("/api/infer/cache/clear");
                    if (res && res->status == 200) {
                        std::cout << "[AUTO_BACKFILL] Cache cleared" << std::endl;
                    }
                } catch (...) {}
                
                // Ждём до следующего интервала
                std::this_thread::sleep_for(std::chrono::seconds(60));
            }
            
            // Проверяем каждую секунду
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    
public:
    void start(const std::vector<std::string>& syms = {}) {
        if (!syms.empty()) {
            symbols = syms;
        }
        running.store(true);
        backfill_thread = std::thread([this](){ backfill_loop(); });
    }
    
    void stop() {
        running.store(false);
        if (backfill_thread.joinable()) {
            backfill_thread.join();
        }
    }
    
    ~AutoBackfill() {
        stop();
    }
};

inline AutoBackfill& get_auto_backfill() {
    static AutoBackfill instance;
    return instance;
}

} // namespace etai
