#pragma once
#include "../json.hpp"
#include <thread>
#include <chrono>
#include <iostream>
#include <atomic>
#include <map>

using json = nlohmann::json;

namespace robot {

extern double get_balance(const std::string& apiKey, const std::string& apiSecret);
extern json get_position(const std::string& apiKey, const std::string& apiSecret, const std::string& symbol);
extern bool open_trade(const std::string& apiKey, const std::string& apiSecret,
                      const std::string& symbol, const std::string& side, double qty,
                      double tp_price, double sl_price, int leverage);
extern bool update_stop_loss(const std::string& apiKey, const std::string& apiSecret,
                             const std::string& symbol, double new_sl_price);

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
    bool auto_trade = false;
    
    // Breakeven настройки
    double breakeven_trigger_percent = 75.0;  // При 75% пути к TP активируем breakeven
    double breakeven_offset_percent = 0.3;    // Переносим SL на entry + 0.3%
};

// Структура для отслеживания позиции
struct PositionTracking {
    std::string side;          // "Buy" или "Sell"
    double entry_price = 0.0;
    double tp_price = 0.0;
    double sl_price = 0.0;
    bool breakeven_applied = false;
};

static std::atomic<bool> robot_running{false};
static std::thread* robot_thread = nullptr;
static RobotConfig config;
static std::map<std::string, PositionTracking> position_tracker;  // symbol -> tracking

void trading_loop(const std::string& apiKey, const std::string& apiSecret) {
    std::cout << "[ROBOT_LOOP] Started for " << config.symbol << std::endl;
    std::cout << "[ROBOT_LOOP] Config: leverage=" << config.leverage
              << " balance=" << config.balance_percent << "% TP=" << config.tp_percent
              << "% SL=" << config.sl_percent << "% auto_trade="
              << (config.auto_trade ? "ON" : "OFF") << std::endl;
    std::cout << "[ROBOT_LOOP] Breakeven: trigger=" << config.breakeven_trigger_percent 
              << "% offset=+" << config.breakeven_offset_percent << "%" << std::endl;

    while (robot_running) {
        try {
            // 1. Проверяем есть ли позиция
            auto position = get_position(apiKey, apiSecret, config.symbol);
            
            if (!position.is_null()) {
                // ПОЗИЦИЯ СУЩЕСТВУЕТ - МОНИТОРИМ BREAKEVEN
                
                // Получаем данные позиции
                std::string side = position.value("side", "");
                double entry_price = std::stod(position.value("avgPrice", "0"));
                double current_price = std::stod(position.value("markPrice", "0"));
                
                // Проверяем есть ли tracking для этой позиции
                auto it = position_tracker.find(config.symbol);
                if (it == position_tracker.end()) {
                    // Первый раз видим эту позицию - инициализируем tracking
                    PositionTracking track;
                    track.side = side;
                    track.entry_price = entry_price;
                    
                    // Пытаемся получить TP/SL из позиции
                    std::string tp_str = position.value("takeProfit", "");
                    std::string sl_str = position.value("stopLoss", "");
                    
                    if (!tp_str.empty() && tp_str != "0") {
                        track.tp_price = std::stod(tp_str);
                    }
                    if (!sl_str.empty() && sl_str != "0") {
                        track.sl_price = std::stod(sl_str);
                    }
                    
                    track.breakeven_applied = false;
                    position_tracker[config.symbol] = track;
                    
                    std::cout << "[ROBOT_LOOP] Position detected: " << side << " @ $" << entry_price 
                              << " TP=$" << track.tp_price << " SL=$" << track.sl_price << std::endl;
                }
                
                // Получаем tracking
                PositionTracking& track = position_tracker[config.symbol];
                
                // BREAKEVEN ЛОГИКА
                if (!track.breakeven_applied && track.tp_price > 0 && track.entry_price > 0) {
                    
                    double distance_to_tp = 0.0;
                    double progress_percent = 0.0;
                    
                    if (track.side == "Buy") {
                        // LONG: цена должна расти к TP
                        distance_to_tp = track.tp_price - track.entry_price;
                        double current_progress = current_price - track.entry_price;
                        progress_percent = (current_progress / distance_to_tp) * 100.0;
                    } else {
                        // SHORT: цена должна падать к TP
                        distance_to_tp = track.entry_price - track.tp_price;
                        double current_progress = track.entry_price - current_price;
                        progress_percent = (current_progress / distance_to_tp) * 100.0;
                    }
                    
                    std::cout << "[ROBOT_LOOP] Monitoring: " << track.side << " price=$" << current_price 
                              << " progress=" << (int)progress_percent << "% to TP" << std::endl;
                    
                    // Проверяем достигли ли trigger процента
                    if (progress_percent >= config.breakeven_trigger_percent) {
                        
                        // АКТИВИРУЕМ BREAKEVEN!
                        double new_sl = 0.0;
                        
                        if (track.side == "Buy") {
                            // LONG: переносим SL выше entry
                            new_sl = track.entry_price * (1.0 + config.breakeven_offset_percent / 100.0);
                        } else {
                            // SHORT: переносим SL ниже entry
                            new_sl = track.entry_price * (1.0 - config.breakeven_offset_percent / 100.0);
                        }
                        
                        std::cout << "[BREAKEVEN] 🎯 TRIGGER! Progress=" << (int)progress_percent 
                                  << "% (>=" << config.breakeven_trigger_percent << "%)" << std::endl;
                        std::cout << "[BREAKEVEN] Moving SL: $" << track.sl_price << " → $" << new_sl 
                                  << " (entry + " << config.breakeven_offset_percent << "%)" << std::endl;
                        
                        // Обновляем SL через Bybit API
                        bool success = update_stop_loss(apiKey, apiSecret, config.symbol, new_sl);
                        
                        if (success) {
                            track.breakeven_applied = true;
                            track.sl_price = new_sl;
                            std::cout << "[BREAKEVEN] ✅ Breakeven activated! Position protected." << std::endl;
                        } else {
                            std::cout << "[BREAKEVEN] ❌ Failed to update SL, will retry next cycle" << std::endl;
                        }
                    }
                }
                
                std::this_thread::sleep_for(std::chrono::seconds(config.check_interval_sec));
                continue;
            } else {
                // НЕТ ПОЗИЦИИ - очищаем tracking если был
                if (position_tracker.find(config.symbol) != position_tracker.end()) {
                    std::cout << "[ROBOT_LOOP] Position closed, clearing tracking" << std::endl;
                    position_tracker.erase(config.symbol);
                }
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
            double last_close = signal.value("last_close", 0.0);

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

            // Проверяем флаг auto_trade
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

            // 5. Получаем готовые TP/SL цены из API
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

            // 6. Открываем сделку
            std::cout << "[ROBOT_LOOP] 🚀 Opening " << side << " " << qty << " @ $" << last_close
                      << " (confidence: " << confidence << "%)" << std::endl;
            std::cout << "[ROBOT_LOOP]    TP: $" << tp_price << " SL: $" << sl_price << std::endl;

            bool success = open_trade(apiKey, apiSecret, config.symbol, side, qty,
                                     tp_price, sl_price, config.leverage);

            if (success) {
                std::cout << "[ROBOT_LOOP] ✅ Trade opened successfully!" << std::endl;
                
                // Инициализируем tracking для новой позиции
                PositionTracking track;
                track.side = side;
                track.entry_price = last_close;
                track.tp_price = tp_price;
                track.sl_price = sl_price;
                track.breakeven_applied = false;
                position_tracker[config.symbol] = track;
                
            } else {
                std::cout << "[ROBOT_LOOP] ❌ Trade failed" << std::endl;
            }

            // 7. После открытия ждём дольше (5 минут)
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
        for (int i = 0; i < 50 && robot_running; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        delete robot_thread;
        robot_thread = nullptr;
    }
    
    position_tracker.clear();
}

bool is_running() {
    return robot_running;
}

RobotConfig get_config() {
    return config;
}

} // namespace robot
