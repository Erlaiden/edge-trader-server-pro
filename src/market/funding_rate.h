#pragma once
#include <string>
#include <curl/curl.h>
#include "../json.hpp"
#include <iostream>

using json = nlohmann::json;

namespace etai {

struct FundingRateData {
    double funding_rate;           // Текущий funding rate
    double funding_rate_8h_ago;    // 8 часов назад
    std::string signal;            // "bullish", "bearish", "neutral", "extreme_bullish", "extreme_bearish"
    double confidence_boost;       // Бонус/штраф к confidence
    bool data_available;
};

// Callback для CURL
static size_t FundingWriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    userp->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// Получение Funding Rate с Bybit
inline FundingRateData get_funding_rate(const std::string& symbol) {
    FundingRateData fr;
    fr.data_available = false;
    fr.confidence_boost = 0.0;
    fr.funding_rate = 0.0;
    
    CURL* curl = curl_easy_init();
    if (!curl) {
        fr.signal = "neutral";
        return fr;
    }
    
    // Bybit API endpoint для Funding Rate History
    std::string url = "https://api.bybit.com/v5/market/funding/history?category=linear&symbol=" 
                    + symbol + "&limit=3";
    
    std::string response_data;
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, FundingWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK) {
        fr.signal = "neutral";
        return fr;
    }
    
    try {
        json j = json::parse(response_data);
        
        if (!j.contains("result") || !j["result"].contains("list") || j["result"]["list"].empty()) {
            fr.signal = "neutral";
            return fr;
        }
        
        auto list = j["result"]["list"];
        
        // Последний funding rate (самый свежий)
        fr.funding_rate = std::stod(list[0]["fundingRate"].get<std::string>());
        
        // 8 часов назад (если есть)
        if (list.size() > 1) {
            fr.funding_rate_8h_ago = std::stod(list[1]["fundingRate"].get<std::string>());
        } else {
            fr.funding_rate_8h_ago = fr.funding_rate;
        }
        
        fr.data_available = true;
        
        // =====================================================================
        // ИНТЕРПРЕТАЦИЯ FUNDING RATE
        // =====================================================================
        
        // Funding Rate > 0: Лонги платят шортам (рынок перекуплен)
        // Funding Rate < 0: Шорты платят лонгам (рынок перепродан)
        
        // Экстремальные уровни для контрариан-стратегии
        if (fr.funding_rate > 0.002) {  // >0.2%
            // ЭКСТРЕМАЛЬНО высокий funding - все в лонгах
            fr.signal = "extreme_bearish";  // Контрариан: пора шортить
            fr.confidence_boost = 15.0;
            
        } else if (fr.funding_rate > 0.001) {  // >0.1%
            // Высокий funding - много лонгов
            fr.signal = "bearish";
            fr.confidence_boost = 8.0;
            
        } else if (fr.funding_rate < -0.002) {  // <-0.2%
            // ЭКСТРЕМАЛЬНО низкий funding - все в шортах
            fr.signal = "extreme_bullish";  // Контрариан: пора лонговать
            fr.confidence_boost = 15.0;
            
        } else if (fr.funding_rate < -0.001) {  // <-0.1%
            // Низкий funding - много шортов
            fr.signal = "bullish";
            fr.confidence_boost = 8.0;
            
        } else {
            // Нормальный funding (-0.1% до +0.1%)
            fr.signal = "neutral";
            fr.confidence_boost = 0.0;
        }
        
        std::cout << "[FUNDING] Rate: " << (fr.funding_rate * 100) << "% | Signal: " << fr.signal << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "[FUNDING] Error: " << e.what() << std::endl;
        fr.signal = "neutral";
        fr.data_available = false;
    }
    
    return fr;
}

// Применение funding к confidence в зависимости от сигнала
inline double apply_funding_boost(const FundingRateData& fr, const std::string& signal) {
    if (!fr.data_available) return 0.0;
    
    double boost = 0.0;
    
    // Контрариан-подход: экстремальный funding = разворот
    if (fr.signal == "extreme_bearish" && signal == "SHORT") {
        boost = fr.confidence_boost;
        std::cout << "[FUNDING] 🔥 EXTREME high funding → SHORT signal confirmed!" << std::endl;
    }
    else if (fr.signal == "extreme_bullish" && signal == "LONG") {
        boost = fr.confidence_boost;
        std::cout << "[FUNDING] 🔥 EXTREME low funding → LONG signal confirmed!" << std::endl;
    }
    else if (fr.signal == "bearish" && signal == "SHORT") {
        boost = fr.confidence_boost;
        std::cout << "[FUNDING] High funding supports SHORT" << std::endl;
    }
    else if (fr.signal == "bullish" && signal == "LONG") {
        boost = fr.confidence_boost;
        std::cout << "[FUNDING] Low funding supports LONG" << std::endl;
    }
    
    return boost;
}

} // namespace etai
