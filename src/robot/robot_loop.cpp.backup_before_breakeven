#pragma once
#include "../json.hpp"
#include <thread>
#include <chrono>
#include <iostream>
#include <atomic>

using json = nlohmann::json;

namespace robot {

extern double get_balance(const std::string& apiKey, const std::string& apiSecret);
extern json get_position(const std::string& apiKey, const std::string& apiSecret, const std::string& symbol);
extern bool open_trade(const std::string& apiKey, const std::string& apiSecret,
                      const std::string& symbol, const std::string& side, double qty,
                      double tp_price, double sl_price, int leverage);

// Внешняя функция для получения сигнала из основного кода
extern json etai_get_signal(const std::string& symbol);

struct RobotConfig {
    std::string symbol = "AIAUSDT";
    int leverage = 10;
    double balance_percent = 90.0;
    double tp_percent = 2.0;
    double sl_percent = 1.0;
    double min_confidence = 60.0;
    int check_interval_sec = 60;
    bool auto_trade = false;  // NEW: флаг автоматической торговли
};

static std::atomic<bool> robot_running{false};
static std::thread* robot_thread = nullptr;
static RobotConfig config;

void trading_loop(const std::string& apiKey, const std::string& apiSecret) {
    std::cout << "[ROBOT_LOOP] Started for " << config.symbol << std::endl;
    std::cout << "[ROBOT_LOOP] Config: leverage=" << config.leverage
              << " balance=" << config.balance_percent << "% TP=" << config.tp_percent
              << "% SL=" << config.sl_percent << "% auto_trade=" 
              << (config.auto_trade ? "ON" : "OFF") << std::endl;

    while (robot_running) {
        try {
            // 1. Проверяем есть ли позиция
            auto position = get_position(apiKey, apiSecret, config.symbol);
            if (!position.is_null()) {
                std::cout << "[ROBOT_LOOP] Position exists, monitoring..." << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(config.check_interval_sec));
                continue;
            }

            // 2. Получаем сигнал
            auto signal = etai_get_signal(config.symbol);
            if (signal.is_null() || !signal.contains("signal")) {
                std::cout << "[ROBOT_LOOP] No signal data" << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(config.check_interval_sec));
                continue;
            }

            std::string signal_type = signal.value("signal", "NEUTRAL");
            double confidence = signal.value("confidence", 0.0);
            double last_close = signal.value("last_close", 0.0);  // FIXED: используем last_close вместо price

            // Логируем сигнал
            std::cout << "[ROBOT_LOOP] Signal: " << signal_type 
                      << " confidence=" << confidence << "% price=" << last_close << std::endl;

            if (signal_type == "NEUTRAL") {
                std::cout << "[ROBOT_LOOP] Signal: HOLD" << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(config.check_interval_sec));
                continue;
            }

            if (confidence < config.min_confidence) {
                std::cout << "[ROBOT_LOOP] Low confidence: " << confidence << "%" << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(config.check_interval_sec));
                continue;
            }

            if (last_close <= 0) {
                std::cout << "[ROBOT_LOOP] Invalid price: " << last_close << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(config.check_interval_sec));
                continue;
            }

            // FIXED: Проверяем флаг auto_trade
            if (!config.auto_trade) {
                std::cout << "[ROBOT_LOOP] 🎯 Trading signal detected but auto_trade=OFF" << std::endl;
                std::cout << "[ROBOT_LOOP]    Signal: " << signal_type << " @ $" << last_close 
                          << " (confidence: " << confidence << "%)" << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(config.check_interval_sec));
                continue;
            }

            // 3. Получаем баланс
            double balance = get_balance(apiKey, apiSecret);
            if (balance < 1.0) {
                std::cout << "[ROBOT_LOOP] Insufficient balance: $" << balance << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(300));
                continue;
            }

            // 4. Рассчитываем размер позиции
            double usable = balance * (config.balance_percent / 100.0);
            double position_value = usable * config.leverage;
            double qty = std::floor(position_value / last_close);

            if (qty < 1.0) {
                std::cout << "[ROBOT_LOOP] Qty too small: " << qty << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(300));
                continue;
            }

            // FIXED: Получаем готовые TP/SL цены из API
            std::string side = signal_type == "LONG" ? "Buy" : "Sell";
            double tp_price = 0.0;
            double sl_price = 0.0;

            if (signal_type == "LONG") {
                tp_price = signal.value("tp_price_long", 0.0);
                sl_price = signal.value("sl_price_long", 0.0);
            } else {
                tp_price = signal.value("tp_price_short", 0.0);
                sl_price = signal.value("sl_price_short", 0.0);
            }

            if (tp_price <= 0 || sl_price <= 0) {
                std::cout << "[ROBOT_LOOP] Invalid TP/SL from signal: TP=" << tp_price 
                          << " SL=" << sl_price << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(config.check_interval_sec));
                continue;
            }

            // 5. Открываем сделку
            std::cout << "[ROBOT_LOOP] 🚀 Opening " << side << " " << qty << " @ $" << last_close
                      << " (confidence: " << confidence << "%)" << std::endl;
            std::cout << "[ROBOT_LOOP]    TP: $" << tp_price << " SL: $" << sl_price << std::endl;

            bool success = open_trade(apiKey, apiSecret, config.symbol, side, qty,
                                     tp_price, sl_price, config.leverage);

            if (success) {
                std::cout << "[ROBOT_LOOP] ✅ Trade opened successfully!" << std::endl;
            } else {
                std::cout << "[ROBOT_LOOP] ❌ Trade failed" << std::endl;
            }

            // 6. После открытия ждём дольше (5 минут)
            std::this_thread::sleep_for(std::chrono::seconds(300));

        } catch (std::exception& e) {
            std::cout << "[ROBOT_LOOP] Exception: " << e.what() << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(60));
        }
    }

    std::cout << "[ROBOT_LOOP] Stopped" << std::endl;
}

bool start(const std::string& apiKey, const std::string& apiSecret, const RobotConfig& cfg) {
    if (robot_running) {
        std::cout << "[ROBOT_LOOP] Already running" << std::endl;
        return false;
    }

    config = cfg;
    robot_running = true;
    robot_thread = new std::thread(trading_loop, apiKey, apiSecret);
    robot_thread->detach();

    return true;
}

void stop() {
    robot_running = false;
    std::cout << "[ROBOT_LOOP] Stop requested" << std::endl;

    if (robot_thread) {
        // Ждём до 5 секунд
        for (int i = 0; i < 50 && robot_running; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        delete robot_thread;
        robot_thread = nullptr;
    }
}

bool is_running() {
    return robot_running;
}

RobotConfig get_config() {
    return config;
}

} // namespace robot
