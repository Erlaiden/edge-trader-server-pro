#pragma once
#include <string>
#include <curl/curl.h>
#include "../json.hpp"
#include <iostream>
#include <ctime>

using json = nlohmann::json;

namespace etai {

struct OpenInterestData {
    double open_interest;
    double oi_24h_ago;
    double oi_change_percent;
    double oi_trend;
    std::string signal;
    double confidence_boost;
    bool data_available;
};

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    userp->append((char*)contents, size * nmemb);
    return size * nmemb;
}

inline OpenInterestData get_open_interest(const std::string& symbol, const std::string& interval = "1h") {
    OpenInterestData oi;
    oi.data_available = false;
    oi.confidence_boost = 0.0;

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "[OI] Failed to initialize CURL" << std::endl;
        oi.signal = "neutral";
        return oi;
    }

    std::string url = "https://api.bybit.com/v5/market/open-interest?category=linear&symbol="
                    + symbol + "&intervalTime=" + interval + "&limit=25";

    std::string response_data;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        std::cerr << "[OI] CURL failed: " << curl_easy_strerror(res) << std::endl;
        oi.signal = "neutral";
        return oi;
    }

    try {
        json j = json::parse(response_data);

        if (!j.contains("result") || !j["result"].contains("list") || j["result"]["list"].empty()) {
            std::cerr << "[OI] Invalid response format" << std::endl;
            oi.signal = "neutral";
            return oi;
        }

        auto list = j["result"]["list"];

        oi.open_interest = std::stod(list[0]["openInterest"].get<std::string>());

        size_t idx_24h = std::min((size_t)24, list.size() - 1);
        oi.oi_24h_ago = std::stod(list[idx_24h]["openInterest"].get<std::string>());

        if (oi.oi_24h_ago > 0) {
            oi.oi_change_percent = ((oi.open_interest - oi.oi_24h_ago) / oi.oi_24h_ago) * 100.0;
        } else {
            oi.oi_change_percent = 0.0;
        }

        double oi_5h_ago = std::stod(list[std::min((size_t)5, list.size()-1)]["openInterest"].get<std::string>());
        oi.oi_trend = (oi.open_interest - oi_5h_ago) / (oi_5h_ago > 0 ? oi_5h_ago : 1.0);

        oi.data_available = true;

        if (oi.oi_change_percent > 15.0) {
            oi.signal = "strong_bullish";
            oi.confidence_boost = 20.0;
        } else if (oi.oi_change_percent > 5.0) {
            oi.signal = "bullish";
            oi.confidence_boost = 12.0;
        } else if (oi.oi_change_percent < -15.0) {
            oi.signal = "strong_bearish";
            oi.confidence_boost = -15.0;
        } else if (oi.oi_change_percent < -5.0) {
            oi.signal = "bearish";
            oi.confidence_boost = -8.0;
        } else {
            oi.signal = "neutral";
            oi.confidence_boost = 0.0;
        }

        std::cout << "[OI] Symbol: " << symbol
                  << " | Current: " << (oi.open_interest / 1e6) << "M"
                  << " | 24h change: " << oi.oi_change_percent << "%"
                  << " | Signal: " << oi.signal << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "[OI] Parse error: " << e.what() << std::endl;
        oi.signal = "neutral";
        oi.data_available = false;
    }

    return oi;
}

inline double analyze_oi_with_price(
    const OpenInterestData& oi,
    double price_change_24h,
    const std::string& signal
) {
    if (!oi.data_available) {
        return 0.0;
    }

    double boost = 0.0;

    // 🚨 КРИТИЧЕСКАЯ ПРОВЕРКА №1: OI >100% - НАИВЫСШИЙ ПРИОРИТЕТ!
    if (std::abs(oi.oi_change_percent) > 100.0) {
        if (oi.oi_change_percent > 100.0 && price_change_24h > 0) {
            if (signal == "LONG") {
                boost = 30.0;
                std::cout << "[OI+PRICE] 🔥 EXTREME PUMP: OI +" << oi.oi_change_percent << "% → LONG ONLY!" << std::endl;
            } else if (signal == "SHORT") {
                boost = -50.0;
                std::cout << "[OI+PRICE] 🚫 BLOCKING SHORT: OI +" << oi.oi_change_percent << "% (PUMP)" << std::endl;
            }
        } else if (oi.oi_change_percent > 100.0) {
            boost = -30.0;
            std::cout << "[OI+PRICE] ⚠️ ANOMALY: OI +" << oi.oi_change_percent << "% but price down" << std::endl;
        } else {
            boost = -30.0;
            std::cout << "[OI+PRICE] ⚠️ EXTREME CRASH: OI " << oi.oi_change_percent << "%" << std::endl;
        }
        return boost;
    }

    // 🔴 ПРОВЕРКА №2: OI 20-100%
    if (std::abs(oi.oi_change_percent) > 20.0) {
        boost = -15.0;
        std::cout << "[OI+PRICE] ⚠️ HIGH OI: " << oi.oi_change_percent << "%" << std::endl;
        return boost;
    }

    // Обычные проверки (OI <20%)
    if (oi.oi_change_percent > 5.0 && price_change_24h > 2.0) {
        if (signal == "LONG") {
            boost = 25.0;
            std::cout << "[OI+PRICE] 🚀 BULLISH: OI+" << oi.oi_change_percent << "% Price+" << price_change_24h << "%" << std::endl;
        } else if (signal == "SHORT") {
            boost = -15.0;
            std::cout << "[OI+PRICE] ⚠️ Contradiction: OI bullish but SHORT" << std::endl;
        }
    }
    else if (oi.oi_change_percent > 5.0 && price_change_24h < -2.0) {
        if (signal == "SHORT") {
            boost = 25.0;
            std::cout << "[OI+PRICE] 🔻 BEARISH: OI+" << oi.oi_change_percent << "% Price-" << std::abs(price_change_24h) << "%" << std::endl;
        } else if (signal == "LONG") {
            boost = -15.0;
            std::cout << "[OI+PRICE] ⚠️ Contradiction: OI bearish but LONG" << std::endl;
        }
    }
    else if (oi.oi_change_percent < -3.0 && price_change_24h > 2.0) {
        if (signal == "LONG") {
            boost = 18.0;
            std::cout << "[OI+PRICE] 💥 SHORT SQUEEZE!" << std::endl;
        }
    }
    else if (oi.oi_change_percent < -3.0 && price_change_24h < -2.0) {
        if (signal == "SHORT") {
            boost = 18.0;
            std::cout << "[OI+PRICE] 💥 LONG LIQUIDATION!" << std::endl;
        }
    }

    return boost;
}

} // namespace etai
