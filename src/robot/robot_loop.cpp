#pragma once
#include "../json.hpp"
#include "../robot/db_helper.cpp"
#include <thread>
#include <chrono>
#include <iostream>
#include <atomic>
#include <map>
#include "../market/dynamic_calibration.h"

using json = nlohmann::json;

namespace robot {

extern double get_balance(const std::string& apiKey, const std::string& apiSecret);
extern json get_position(const std::string& apiKey, const std::string& apiSecret, const std::string& symbol);
extern bool open_trade(const std::string& apiKey, const std::string& apiSecret,
                      const std::string& symbol, const std::string& side, double qty,
                      double tp_percent, double sl_percent, int leverage);
extern bool update_stop_loss(const std::string& apiKey, const std::string& apiSecret,
                             const std::string& symbol, double new_sl_price);

extern json etai_get_signal(const std::string& symbol);

struct RobotConfig {
    std::string symbol = "AIAUSDT";
    int leverage = 10;
    double balance_percent = 90.0;
    double tp_percent = 2.0;
    double sl_percent = 1.0;
    double min_confidence = 25.0;
    int check_interval_sec = 60;
    bool auto_trade = false;

    double breakeven_trigger_percent = 75.0;
    double breakeven_offset_percent = 0.3;
};

struct PositionTracking {
    int user_id = 0;
    int trade_id = -1;
    std::string side;
    double entry_price = 0.0;
    double tp_price = 0.0;
    double sl_price = 0.0;
    double qty = 0.0;
    bool breakeven_applied = false;
};

static std::atomic<bool> robot_running{false};
static std::thread* robot_thread = nullptr;
static RobotConfig config;
static int current_user_id = 0;
static std::map<std::string, PositionTracking> position_tracker;

void trading_loop(const std::string& apiKey, const std::string& apiSecret) {
    std::cout << "[ROBOT_LOOP] Started for " << config.symbol << " user_id=" << current_user_id << std::endl;
    std::cout << "[ROBOT_LOOP] Config: leverage=" << config.leverage
              << " balance=" << config.balance_percent << "% TP=" << config.tp_percent
              << "% SL=" << config.sl_percent << "% auto_trade="
              << (config.auto_trade ? "ON" : "OFF") << std::endl;
    std::cout << "[ROBOT_LOOP] Breakeven: trigger=" << config.breakeven_trigger_percent
              << "% offset=+" << config.breakeven_offset_percent << "%" << std::endl;

    while (robot_running) {
        try {
            auto position = get_position(apiKey, apiSecret, config.symbol);

            if (!position.is_null()) {
                // ПОЗИЦИЯ СУЩЕСТВУЕТ

                std::string side = position.value("side", "");
                double entry_price = std::stod(position.value("avgPrice", "0"));
                double current_price = std::stod(position.value("markPrice", "0"));

                auto it = position_tracker.find(config.symbol);
                if (it == position_tracker.end()) {
                    PositionTracking track;
                    track.user_id = current_user_id;
                    track.side = side;
                    track.entry_price = entry_price;

                    std::string tp_str = position.value("takeProfit", "");
                    std::string sl_str = position.value("stopLoss", "");

                    if (!tp_str.empty() && tp_str != "0") {
                        track.tp_price = std::stod(tp_str);
                    }
                    if (!sl_str.empty() && sl_str != "0") {
                        track.sl_price = std::stod(sl_str);
                    }

                    track.breakeven_applied = false;

                    track.trade_id = db::get_open_trade_id(current_user_id, config.symbol);

                    position_tracker[config.symbol] = track;

                    std::cout << "[ROBOT_LOOP] Position detected: " << side << " @ $" << entry_price
                              << " TP=$" << track.tp_price << " SL=$" << track.sl_price
                              << " trade_id=" << track.trade_id << std::endl;
                }

                PositionTracking& track = position_tracker[config.symbol];

                // BREAKEVEN ЛОГИКА
                if (!track.breakeven_applied && track.tp_price > 0 && track.entry_price > 0) {

                    double distance_to_tp = 0.0;
                    double progress_percent = 0.0;

                    if (track.side == "Buy") {
                        distance_to_tp = track.tp_price - track.entry_price;
                        double current_progress = current_price - track.entry_price;
                        progress_percent = (current_progress / distance_to_tp) * 100.0;
                    } else {
                        distance_to_tp = track.entry_price - track.tp_price;
                        double current_progress = track.entry_price - current_price;
                        progress_percent = (current_progress / distance_to_tp) * 100.0;
                    }

                    std::cout << "[ROBOT_LOOP] Monitoring: " << track.side << " price=$" << current_price
                              << " progress=" << (int)progress_percent << "% to TP" << std::endl;

                    if (progress_percent >= config.breakeven_trigger_percent) {

                        double new_sl = 0.0;

                        if (track.side == "Buy") {
                            new_sl = track.entry_price * (1.0 + config.breakeven_offset_percent / 100.0);
                        } else {
                            new_sl = track.entry_price * (1.0 - config.breakeven_offset_percent / 100.0);
                        }

                        std::cout << "[BREAKEVEN] 🎯 TRIGGER! Progress=" << (int)progress_percent
                                  << "% (>=" << config.breakeven_trigger_percent << "%)" << std::endl;
                        std::cout << "[BREAKEVEN] Moving SL: $" << track.sl_price << " → $" << new_sl
                                  << " (entry + " << config.breakeven_offset_percent << "%)" << std::endl;

                        bool success = update_stop_loss(apiKey, apiSecret, config.symbol, new_sl);

                        if (success) {
                            track.breakeven_applied = true;
                            track.sl_price = new_sl;

                            if (track.trade_id > 0) {
                                db::mark_breakeven_activated(track.trade_id);
                            }

                            std::cout << "[BREAKEVEN] ✅ Breakeven activated! Position protected." << std::endl;
                        } else {
                            std::cout << "[BREAKEVEN] ❌ Failed to update SL, will retry next cycle" << std::endl;
                        }
                    }
                }

                std::this_thread::sleep_for(std::chrono::seconds(config.check_interval_sec));
                continue;

            } else {
                // НЕТ ПОЗИЦИИ
                if (position_tracker.find(config.symbol) != position_tracker.end()) {

                    PositionTracking& track = position_tracker[config.symbol];

                    std::cout << "[ROBOT_LOOP] Position closed, clearing tracking" << std::endl;

                    if (track.trade_id > 0) {
                        auto signal = etai_get_signal(config.symbol);
                        double exit_price = signal.value("last_close", track.entry_price);
                        double pnl = 0.0;

                        if (track.side == "Buy") {
                            pnl = (exit_price - track.entry_price) * track.qty;
                        } else {
                            pnl = (track.entry_price - exit_price) * track.qty;
                        }

                        db::update_trade(track.trade_id, exit_price, pnl, "closed", "position_closed");
                        std::cout << "[ROBOT_LOOP] Trade closed: id=" << track.trade_id << " exit=" << exit_price << " pnl=" << pnl << std::endl;
                        
                        // =====================================================================
                        // 📊 AUTO-CALIBRATION: Record trade result
                        // =====================================================================
                        try {
                            double pnl_percent = (pnl / (track.entry_price * track.qty)) * 100.0;
                            
                            etai::TradeResult trade_result;
                            trade_result.timestamp = std::to_string(std::time(nullptr));
                            trade_result.symbol = config.symbol;
                            trade_result.signal = track.side == "Buy" ? "LONG" : "SHORT";
                            trade_result.entry_price = track.entry_price;
                            trade_result.exit_price = exit_price;
                            trade_result.pnl_percent = pnl_percent;
                            trade_result.is_win = pnl > 0;
                            trade_result.confidence = 0.0;  // TODO: сохранять при открытии
                            trade_result.threshold = 0.0;   // TODO: сохранять при открытии
                            
                            etai::g_calibrator.add_trade(trade_result);
                            
                            std::cout << "[CALIBRATION] ✅ Trade recorded: " << trade_result.signal 
                                      << " PnL=" << pnl_percent << "% Win=" << trade_result.is_win 
                                      << " | Total: " << etai::g_calibrator.get_trade_count() << " trades" << std::endl;
                            
                            // Показываем статистику если достаточно сделок
                            if (etai::g_calibrator.get_trade_count() >= 5) {
                                auto stats = etai::g_calibrator.get_stats();
                                std::cout << "[CALIBRATION] 📊 Win Rate: " << (stats["win_rate"].get<double>() * 100) 
                                          << "% | Avg PnL: " << stats["avg_pnl"].get<double>() << "%" << std::endl;
                            }
                        } catch (const std::exception& e) {
                            std::cerr << "[CALIBRATION] ❌ Error: " << e.what() << std::endl;
                        }
                    }

                    position_tracker.erase(config.symbol);
                }
            }

            // Получаем сигнал
            auto signal = etai_get_signal(config.symbol);
            if (signal.is_null() || !signal.contains("signal")) {
                std::cout << "[ROBOT_LOOP] No signal data" << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(config.check_interval_sec));
                continue;
            }

            std::string signal_type = signal.value("signal", "NEUTRAL");
            double confidence = signal.value("confidence", 0.0);
            // ✅ ДИНАМИЧЕСКИЕ TP/SL ИЗ AI
            double dynamic_tp = signal.value("tp", config.tp_percent / 100.0) * 100.0;
            double dynamic_sl = signal.value("sl", config.sl_percent / 100.0) * 100.0;
            double atr_percent = signal.value("atr_percent", 0.0);
            std::cout << "[ROBOT_LOOP] ATR: " << atr_percent << "% → TP: " << dynamic_tp << "% SL: " << dynamic_sl << "%" << std::endl;

            double last_close = signal.value("last_close", 0.0);

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

            if (!config.auto_trade) {
                std::cout << "[ROBOT_LOOP] 🎯 Trading signal detected but auto_trade=OFF" << std::endl;
                std::cout << "[ROBOT_LOOP]    Signal: " << signal_type << " @ $" << last_close
                          << " (confidence: " << confidence << "%)" << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(config.check_interval_sec));
                continue;
            }

            double balance = get_balance(apiKey, apiSecret);
            if (balance < 1.0) {
                std::cout << "[ROBOT_LOOP] Insufficient balance: $" << balance << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(300));
                continue;
            }

            double usable = balance * (config.balance_percent / 100.0);
            double position_value = usable * config.leverage;
            double qty = std::floor(position_value / last_close);

            if (qty < 1.0) {
                std::cout << "[ROBOT_LOOP] Qty too small: " << qty << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(300));
                continue;
            }

            std::string side = signal_type == "LONG" ? "Buy" : "Sell";

            std::cout << "[ROBOT_LOOP] 🚀 Opening " << side << " " << qty << " @ $" << last_close
                      << " (confidence: " << confidence << "%)" << std::endl;
            std::cout << "[ROBOT_LOOP]    TP: " << dynamic_tp << "% SL: " << dynamic_sl << "%" << std::endl;

            bool success = open_trade(apiKey, apiSecret, config.symbol, side, qty,
                                     dynamic_tp, dynamic_sl, config.leverage);

            if (success) {
                std::cout << "[ROBOT_LOOP] ✅ Trade opened successfully!" << std::endl;

                // Получаем реальную позицию чтобы сохранить правильные TP/SL
                auto new_position = get_position(apiKey, apiSecret, config.symbol);
                double real_entry = last_close;
                double real_tp = 0.0;
                double real_sl = 0.0;

                if (!new_position.is_null()) {
                    real_entry = std::stod(new_position.value("avgPrice", std::to_string(last_close)));
                    std::string tp_str = new_position.value("takeProfit", "");
                    std::string sl_str = new_position.value("stopLoss", "");
                    
                    if (!tp_str.empty() && tp_str != "0") {
                        real_tp = std::stod(tp_str);
                    }
                    if (!sl_str.empty() && sl_str != "0") {
                        real_sl = std::stod(sl_str);
                    }
                }

                // Сохраняем в БД
                int trade_id = db::save_trade(current_user_id, config.symbol, side, qty,
                                               real_entry, real_tp, real_sl);

                // Инициализируем tracking
                PositionTracking track;
                track.user_id = current_user_id;
                track.trade_id = trade_id;
                track.side = side;
                track.entry_price = real_entry;
                track.tp_price = real_tp;
                track.sl_price = real_sl;
                track.qty = qty;
                track.breakeven_applied = false;
                position_tracker[config.symbol] = track;

                std::cout << "[ROBOT_LOOP] 📝 Saved to DB with trade_id=" << trade_id << std::endl;

            } else {
                std::cout << "[ROBOT_LOOP] ❌ Trade failed" << std::endl;
            }

            std::this_thread::sleep_for(std::chrono::seconds(300));

        } catch (std::exception& e) {
            std::cout << "[ROBOT_LOOP] Exception: " << e.what() << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(60));
        }
    }

    std::cout << "[ROBOT_LOOP] Stopped" << std::endl;
}

bool start(const std::string& apiKey, const std::string& apiSecret, const RobotConfig& cfg, int user_id) {
    if (robot_running) {
        std::cout << "[ROBOT_LOOP] Already running" << std::endl;
        return false;
    }

    config = cfg;
    current_user_id = user_id;
    robot_running = true;
    robot_thread = new std::thread(trading_loop, apiKey, apiSecret);
    robot_thread->detach();

    return true;
}

bool start(const std::string& apiKey, const std::string& apiSecret, const RobotConfig& cfg) {
    return start(apiKey, apiSecret, cfg, 0);
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
    current_user_id = 0;
}

bool is_running() {
    return robot_running;
}

RobotConfig get_config() {
    return config;
}

} // namespace robot
