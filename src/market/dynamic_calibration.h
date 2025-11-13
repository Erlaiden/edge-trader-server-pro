#pragma once
#include <deque>
#include <string>
#include <fstream>
#include <iostream>
#include "../json.hpp"

using json = nlohmann::json;

namespace etai {

struct TradeResult {
    std::string timestamp;
    std::string symbol;
    std::string signal;
    double      entry_price;
    double      exit_price;
    double      pnl_percent;
    bool        is_win;
    double      confidence;
    double      threshold;
};

// --- безопасные чтения из json, чтобы не падать на null/строках ---

inline std::string safe_string(const json& j, const char* key, const std::string& def = "") {
    if (!j.contains(key)) return def;
    const auto& v = j.at(key);
    if (v.is_string()) return v.get<std::string>();
    // если там число/бул/массив/объект/null — просто сериализуем в строку
    try {
        return v.dump();
    } catch (...) {
        return def;
    }
}

inline double safe_double(const json& j, const char* key, double def = 0.0) {
    if (!j.contains(key)) return def;
    const auto& v = j.at(key);
    if (v.is_number_float() || v.is_number_integer()) {
        return v.get<double>();
    }
    if (v.is_string()) {
        try {
            return std::stod(v.get<std::string>());
        } catch (...) {
            return def;
        }
    }
    // null, bool, объект, массив → дефолт
    return def;
}

inline bool safe_bool(const json& j, const char* key, bool def = false) {
    if (!j.contains(key)) return def;
    const auto& v = j.at(key);
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number_integer()) {
        return v.get<int>() != 0;
    }
    return def;
}

class DynamicCalibration {
private:
    std::deque<TradeResult> last_trades;
    const size_t MAX_HISTORY = 20;
    const std::string HISTORY_FILE = "trade_history.json";

public:
    DynamicCalibration() {
        load_history();
    }

    // Загрузка истории из файла (НЕ падаем на кривых json)
    void load_history() {
        std::ifstream file(HISTORY_FILE);
        if (!file.is_open()) {
            std::cout << "[CALIBRATION] No history file found, starting fresh" << std::endl;
            return;
        }

        try {
            json j;
            file >> j;

            if (!j.is_object() || !j.contains("trades") || !j["trades"].is_array()) {
                std::cout << "[CALIBRATION] History file has unexpected format, ignoring" << std::endl;
                return;
            }

            for (const auto& trade : j["trades"]) {
                if (!trade.is_object()) continue;

                TradeResult tr;
                tr.timestamp    = safe_string(trade, "timestamp", "");
                tr.symbol       = safe_string(trade, "symbol", "");
                tr.signal       = safe_string(trade, "signal", "");
                tr.entry_price  = safe_double(trade, "entry_price", 0.0);
                tr.exit_price   = safe_double(trade, "exit_price", 0.0);
                tr.pnl_percent  = safe_double(trade, "pnl_percent", 0.0);
                tr.is_win       = safe_bool(trade, "is_win", false);
                tr.confidence   = safe_double(trade, "confidence", 0.0);
                tr.threshold    = safe_double(trade, "threshold", 0.0);

                last_trades.push_back(tr);
            }

            std::cout << "[CALIBRATION] Loaded " << last_trades.size()
                      << " trades from history" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[CALIBRATION] Error loading history: " << e.what() << std::endl;
        }
    }

    // Сохранение истории в файл
    void save_history() {
        json j;
        j["trades"] = json::array();

        for (const auto& trade : last_trades) {
            json trade_json;
            trade_json["timestamp"]    = trade.timestamp;
            trade_json["symbol"]       = trade.symbol;
            trade_json["signal"]       = trade.signal;
            trade_json["entry_price"]  = trade.entry_price;
            trade_json["exit_price"]   = trade.exit_price;
            trade_json["pnl_percent"]  = trade.pnl_percent;
            trade_json["is_win"]       = trade.is_win;
            trade_json["confidence"]   = trade.confidence;
            trade_json["threshold"]    = trade.threshold;

            j["trades"].push_back(trade_json);
        }

        std::ofstream file(HISTORY_FILE);
        file << j.dump(2);
    }

    // Добавить сделку в историю
    void add_trade(const TradeResult& trade) {
        last_trades.push_back(trade);

        // Ограничиваем размер истории
        if (last_trades.size() > MAX_HISTORY) {
            last_trades.pop_front();
        }

        save_history();

        std::cout << "[CALIBRATION] Trade added: " << trade.signal
                  << " PnL=" << trade.pnl_percent << "% Win=" << trade.is_win
                  << " | Total trades: " << last_trades.size() << std::endl;
    }

    // Расчет win rate
    double get_win_rate() const {
        if (last_trades.empty()) return 0.5;  // По умолчанию 50%
        int wins = 0;
        for (const auto& trade : last_trades) {
            if (trade.is_win) wins++;
        }
        return static_cast<double>(wins) / last_trades.size();
    }

    // Расчет среднего PnL
    double get_avg_pnl() const {
        if (last_trades.empty()) return 0.0;

        double sum = 0.0;
        for (const auto& trade : last_trades) {
            sum += trade.pnl_percent;
        }
        return sum / last_trades.size();
    }

    // Калиброванный порог на основе win rate
    double get_calibrated_threshold(double base_threshold) const {
        if (last_trades.size() < 10) {
            // Недостаточно данных - используем базовый
            return base_threshold;
        }

        double win_rate = get_win_rate();

        // =====================================================================
        // ЛОГИКА КАЛИБРОВКИ
        // =====================================================================

        if (win_rate > 0.65) {
            // Отличный win rate - СНИЖАЕМ порог (более агрессивно)
            double adjusted = base_threshold * 0.85;
            std::cout << "[CALIBRATION] 🔥 Win rate " << (win_rate * 100.0)
                      << "% - lowering threshold to " << adjusted << "% (aggressive)" << std::endl;
            return adjusted;
        } else if (win_rate > 0.55) {
            // Хороший win rate - немного снижаем
            double adjusted = base_threshold * 0.95;
            std::cout << "[CALIBRATION] ✅ Win rate " << (win_rate * 100.0)
                      << "% - lowering threshold to " << adjusted << "%" << std::endl;
            return adjusted;
        } else if (win_rate < 0.40) {
            // Плохой win rate - ПОВЫШАЕМ порог (более осторожно)
            double adjusted = base_threshold * 1.15;
            std::cout << "[CALIBRATION] ⚠️ Win rate " << (win_rate * 100.0)
                      << "% - raising threshold to " << adjusted << "% (careful)" << std::endl;
            return adjusted;
        } else if (win_rate < 0.50) {
            // Средний win rate - немного повышаем
            double adjusted = base_threshold * 1.05;
            std::cout << "[CALIBRATION] ⚠️ Win rate " << (win_rate * 100.0)
                      << "% - raising threshold to " << adjusted << "%" << std::endl;
            return adjusted;
        }

        // Win rate 50-55% - оставляем как есть
        return base_threshold;
    }

    // Получение статистики
    json get_stats() const {
        json stats;
        stats["total_trades"]         = last_trades.size();
        stats["win_rate"]             = get_win_rate();
        stats["avg_pnl"]              = get_avg_pnl();
        stats["ready_for_calibration"]= last_trades.size() >= 10;

        if (!last_trades.empty()) {
            stats["last_trade_pnl"] = last_trades.back().pnl_percent;
            stats["last_trade_win"] = last_trades.back().is_win;
        }

        return stats;
    }

    size_t get_trade_count() const {
        return last_trades.size();
    }
};

// Глобальный инстанс калибратора (внутренняя линковка через static)
static DynamicCalibration g_calibrator;

} // namespace etai
