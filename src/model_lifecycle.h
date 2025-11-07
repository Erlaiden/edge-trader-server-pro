#pragma once
#include <filesystem>
#include <chrono>
#include <thread>
#include <atomic>
#include <iostream>

namespace etai {

class ModelLifecycle {
private:
    std::atomic<bool> running{false};
    std::thread cleanup_thread;
    
    const int MODEL_MAX_DAYS = 7; // Модель живёт 7 дней
    
    void cleanup_loop() {
        while (running.load()) {
            cleanup_old_models();
            
            // Проверяем раз в час
            for (int i = 0; i < 3600 && running.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }
    
    void cleanup_old_models() {
        namespace fs = std::filesystem;
        auto now = std::chrono::system_clock::now();
        int deleted_models = 0;
        int deleted_data = 0;
        
        try {
            // Очищаем модели старше 7 дней
            if (fs::exists("cache/models")) {
                for (auto& entry : fs::directory_iterator("cache/models")) {
                    if (entry.path().extension() == ".json" && 
                        entry.path().filename().string().find("_ppo_pro.json") != std::string::npos) {
                        
                        auto file_time = fs::last_write_time(entry);
                        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                            file_time - fs::file_time_type::clock::now() + std::chrono::system_clock::now()
                        );
                        
                        auto age_days = std::chrono::duration_cast<std::chrono::hours>(now - sctp).count() / 24;
                        
                        if (age_days > MODEL_MAX_DAYS) {
                            std::cout << "[LIFECYCLE] Deleting old model: " << entry.path().filename() 
                                     << " (age: " << age_days << " days)" << std::endl;
                            fs::remove(entry.path());
                            deleted_models++;
                        }
                    }
                }
            }
            
            // Очищаем данные старше 7 дней
            if (fs::exists("cache/clean")) {
                for (auto& entry : fs::directory_iterator("cache/clean")) {
                    if (entry.path().extension() == ".csv") {
                        auto file_time = fs::last_write_time(entry);
                        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                            file_time - fs::file_time_type::clock::now() + std::chrono::system_clock::now()
                        );
                        
                        auto age_days = std::chrono::duration_cast<std::chrono::hours>(now - sctp).count() / 24;
                        
                        if (age_days > MODEL_MAX_DAYS) {
                            fs::remove(entry.path());
                            deleted_data++;
                        }
                    }
                }
            }
            
            // Очищаем временные файлы старше 1 дня
            if (fs::exists("cache/xy")) {
                for (auto& entry : fs::directory_iterator("cache/xy")) {
                    auto file_time = fs::last_write_time(entry);
                    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                        file_time - fs::file_time_type::clock::now() + std::chrono::system_clock::now()
                    );
                    
                    auto age_days = std::chrono::duration_cast<std::chrono::hours>(now - sctp).count() / 24;
                    
                    if (age_days > 1) {
                        fs::remove(entry.path());
                    }
                }
            }
            
            if (deleted_models > 0 || deleted_data > 0) {
                std::cout << "[LIFECYCLE] Cleanup complete: " 
                         << deleted_models << " models, " 
                         << deleted_data << " data files removed" << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "[LIFECYCLE] Error: " << e.what() << std::endl;
        }
    }
    
public:
    void start() {
        running.store(true);
        cleanup_thread = std::thread([this](){ cleanup_loop(); });
        std::cout << "[LIFECYCLE] Started: models expire after " << MODEL_MAX_DAYS << " days" << std::endl;
    }
    
    void stop() {
        running.store(false);
        if (cleanup_thread.joinable()) {
            cleanup_thread.join();
        }
    }
    
    // Проверить возраст модели
    int get_model_age_days(const std::string& symbol, const std::string& interval) {
        namespace fs = std::filesystem;
        std::string path = "cache/models/" + symbol + "_" + interval + "_ppo_pro.json";
        
        if (!fs::exists(path)) {
            return -1;
        }
        
        try {
            auto file_time = fs::last_write_time(path);
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                file_time - fs::file_time_type::clock::now() + std::chrono::system_clock::now()
            );
            auto now = std::chrono::system_clock::now();
            auto age_days = std::chrono::duration_cast<std::chrono::hours>(now - sctp).count() / 24;
            return (int)age_days;
        } catch (...) {
            return -1;
        }
    }
    
    ~ModelLifecycle() {
        stop();
    }
};

inline ModelLifecycle& get_model_lifecycle() {
    static ModelLifecycle instance;
    return instance;
}

} // namespace etai
