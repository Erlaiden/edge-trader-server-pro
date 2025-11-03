#pragma once
#include <string>
#include <unordered_map>
#include <algorithm>
#include <cctype>

namespace etai {

inline std::string to_upper_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::toupper(c); });
    return s;
}

/*
 * Нормализация тикеров под фактические названия на бирже.
 * Минимально необходимое для нас прямо сейчас:
 *   - MATICUSDT -> POLUSDT  (ребрендинг Polygon: MATIC -> POL)
 * Плюс несколько безопасных исторических синонимов на всякий случай.
 */
inline std::string symbol_normalize(std::string requested) {
    static const std::unordered_map<std::string, std::string> MAP = {
        {"MATICUSDT", "POLUSDT"},
        // Нежёсткие доп. алиасы (безопасные подмены к ликвидным тикерам):
        {"XBTUSDT",   "BTCUSDT"},
        {"BCCUSDT",   "BCHUSDT"}
    };
    std::string up = to_upper_copy(requested);
    auto it = MAP.find(up);
    return (it != MAP.end()) ? it->second : up;
}

} // namespace etai
