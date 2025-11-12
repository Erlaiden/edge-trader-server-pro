#pragma once
#include <string>
#include <vector>
#include <curl/curl.h>
#include "../json.hpp"
#include <iostream>
#include <numeric>

using json = nlohmann::json;

namespace etai {

struct OrderBookLevel {
    double price;
    double quantity;
    double total_value;  // price * quantity
};

struct OrderBookAnalysis {
    // Стены (крупные ордера)
    double buy_wall_strength;    // Сила стены покупок
    double sell_wall_strength;   // Сила стены продаж
    double wall_ratio;           // buy_wall / sell_wall
    
    // Дисбаланс
    double bid_volume;           // Общий объем bid
    double ask_volume;           // Общий объем ask
    double imbalance;            // (bid - ask) / (bid + ask)
    
    // Сигналы
    std::string signal;          // "strong_buy", "buy", "sell", "strong_sell", "neutral"
    double confidence_boost;     // Бонус к confidence
    bool data_available;
    
    // Детали для debug
    double best_bid;
    double best_ask;
    double spread_percent;
};

// Callback для CURL
static size_t OBWriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    userp->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// Получение Order Book с Bybit
inline OrderBookAnalysis get_order_book(const std::string& symbol, int depth = 50) {
    OrderBookAnalysis ob;
    ob.data_available = false;
    ob.confidence_boost = 0.0;
    ob.signal = "neutral";
    
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "[ORDERBOOK] Failed to initialize CURL" << std::endl;
        return ob;
    }
    
    // Bybit Order Book API
    std::string url = "https://api.bybit.com/v5/market/orderbook?category=linear&symbol="
                    + symbol + "&limit=" + std::to_string(depth);
    
    std::string response_data;
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OBWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK) {
        std::cerr << "[ORDERBOOK] CURL failed: " << curl_easy_strerror(res) << std::endl;
        return ob;
    }
    
    try {
        json j = json::parse(response_data);
        
        if (!j.contains("result") || !j["result"].contains("b") || !j["result"].contains("a")) {
            std::cerr << "[ORDERBOOK] Invalid response format" << std::endl;
            return ob;
        }
        
        auto bids = j["result"]["b"];  // [[price, quantity], ...]
        auto asks = j["result"]["a"];
        
        if (bids.empty() || asks.empty()) {
            std::cerr << "[ORDERBOOK] Empty order book" << std::endl;
            return ob;
        }
        
        // =====================================================================
        // АНАЛИЗ ORDER BOOK
        // =====================================================================
        
        // Best bid/ask
        ob.best_bid = std::stod(bids[0][0].get<std::string>());
        ob.best_ask = std::stod(asks[0][0].get<std::string>());
        ob.spread_percent = ((ob.best_ask - ob.best_bid) / ob.best_bid) * 100.0;
        
        // Считаем общий объем
        ob.bid_volume = 0.0;
        ob.ask_volume = 0.0;
        
        for (const auto& bid : bids) {
            double price = std::stod(bid[0].get<std::string>());
            double qty = std::stod(bid[1].get<std::string>());
            ob.bid_volume += price * qty;
        }
        
        for (const auto& ask : asks) {
            double price = std::stod(ask[0].get<std::string>());
            double qty = std::stod(ask[1].get<std::string>());
            ob.ask_volume += price * qty;
        }
        
        // Imbalance: положительный = больше покупателей
        double total = ob.bid_volume + ob.ask_volume;
        if (total > 0) {
            ob.imbalance = (ob.bid_volume - ob.ask_volume) / total;
        } else {
            ob.imbalance = 0.0;
        }
        
        // =====================================================================
        // ДЕТЕКЦИЯ "СТЕН" (крупных ордеров)
        // =====================================================================
        
        // Средний размер ордера
        double avg_bid = ob.bid_volume / bids.size();
        double avg_ask = ob.ask_volume / asks.size();
        
        // Ищем крупные ордера (>3x среднего)
        ob.buy_wall_strength = 0.0;
        ob.sell_wall_strength = 0.0;
        
        for (const auto& bid : bids) {
            double price = std::stod(bid[0].get<std::string>());
            double qty = std::stod(bid[1].get<std::string>());
            double value = price * qty;
            
            if (value > avg_bid * 3.0) {
                ob.buy_wall_strength += value;
            }
        }
        
        for (const auto& ask : asks) {
            double price = std::stod(ask[0].get<std::string>());
            double qty = std::stod(ask[1].get<std::string>());
            double value = price * qty;
            
            if (value > avg_ask * 3.0) {
                ob.sell_wall_strength += value;
            }
        }
        
        // Wall ratio
        if (ob.sell_wall_strength > 0) {
            ob.wall_ratio = ob.buy_wall_strength / ob.sell_wall_strength;
        } else {
            ob.wall_ratio = ob.buy_wall_strength > 0 ? 10.0 : 1.0;
        }
        
        // =====================================================================
        // ГЕНЕРАЦИЯ СИГНАЛА
        // =====================================================================
        
        // 1. СИЛЬНЫЙ дисбаланс (>20%)
        if (ob.imbalance > 0.20) {
            ob.signal = "strong_buy";
            ob.confidence_boost = 20.0;
            std::cout << "[ORDERBOOK] 🟢 STRONG BUY pressure: " << (ob.imbalance*100) 
                      << "% imbalance" << std::endl;
        }
        else if (ob.imbalance < -0.20) {
            ob.signal = "strong_sell";
            ob.confidence_boost = 20.0;
            std::cout << "[ORDERBOOK] 🔴 STRONG SELL pressure: " << (ob.imbalance*100) 
                      << "% imbalance" << std::endl;
        }
        
        // 2. Умеренный дисбаланс (10-20%)
        else if (ob.imbalance > 0.10) {
            ob.signal = "buy";
            ob.confidence_boost = 12.0;
            std::cout << "[ORDERBOOK] 🟢 BUY pressure: " << (ob.imbalance*100) 
                      << "%" << std::endl;
        }
        else if (ob.imbalance < -0.10) {
            ob.signal = "sell";
            ob.confidence_boost = 12.0;
            std::cout << "[ORDERBOOK] 🔴 SELL pressure: " << (ob.imbalance*100) 
                      << "%" << std::endl;
        }
        
        // 3. Анализ СТЕН
        else if (ob.wall_ratio > 2.0) {
            // Большие BUY стены = поддержка
            ob.signal = "buy_wall";
            ob.confidence_boost = 15.0;
            std::cout << "[ORDERBOOK] 🧱 BUY WALL detected: " << ob.wall_ratio 
                      << "x stronger than sell" << std::endl;
        }
        else if (ob.wall_ratio < 0.5) {
            // Большие SELL стены = сопротивление
            ob.signal = "sell_wall";
            ob.confidence_boost = 15.0;
            std::cout << "[ORDERBOOK] 🧱 SELL WALL detected: " 
                      << (1.0/ob.wall_ratio) << "x stronger than buy" << std::endl;
        }
        
        // 4. Нейтральная книга
        else {
            ob.signal = "neutral";
            ob.confidence_boost = 0.0;
        }
        
        ob.data_available = true;
        
        std::cout << "[ORDERBOOK] " << symbol 
                  << " | Spread: " << ob.spread_percent << "%"
                  << " | Imbalance: " << (ob.imbalance*100) << "%"
                  << " | Signal: " << ob.signal << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "[ORDERBOOK] Parse error: " << e.what() << std::endl;
        ob.data_available = false;
    }
    
    return ob;
}

// Применение буста в зависимости от сигнала
inline double apply_orderbook_boost(
    const OrderBookAnalysis& ob,
    const std::string& signal
) {
    if (!ob.data_available) {
        return 0.0;
    }
    
    double boost = 0.0;
    
    // Книга подтверждает направление
    if ((ob.signal == "strong_buy" || ob.signal == "buy" || ob.signal == "buy_wall") && signal == "LONG") {
        boost = ob.confidence_boost;
        std::cout << "[ORDERBOOK] ✅ Confirms LONG signal: +" << boost << "%" << std::endl;
    }
    else if ((ob.signal == "strong_sell" || ob.signal == "sell" || ob.signal == "sell_wall") && signal == "SHORT") {
        boost = ob.confidence_boost;
        std::cout << "[ORDERBOOK] ✅ Confirms SHORT signal: +" << boost << "%" << std::endl;
    }
    // Книга противоречит
    else if ((ob.signal == "strong_buy" || ob.signal == "buy") && signal == "SHORT") {
        boost = -ob.confidence_boost * 0.7;  // Штраф 70%
        std::cout << "[ORDERBOOK] ⚠️ Contradicts SHORT: " << boost << "%" << std::endl;
    }
    else if ((ob.signal == "strong_sell" || ob.signal == "sell") && signal == "LONG") {
        boost = -ob.confidence_boost * 0.7;
        std::cout << "[ORDERBOOK] ⚠️ Contradicts LONG: " << boost << "%" << std::endl;
    }
    
    return boost;
}

} // namespace etai
