#pragma once
#include "../json.hpp"
#include <httplib.h>
#include <string>
#include <openssl/hmac.h>
#include <iomanip>
#include <sstream>

using json = nlohmann::json;

extern std::string hmac_sha256(const std::string& key, const std::string& data);

namespace robot {

static json bybit_request(const std::string& method, const std::string& endpoint,
                          const std::string& apiKey, const std::string& apiSecret,
                          const json& body = json::object(), const std::string& params = "") {
    httplib::SSLClient cli("api.bybit.com");
    cli.set_connection_timeout(10);
    cli.set_read_timeout(10);

    std::string timestamp = std::to_string(std::time(nullptr) * 1000);
    std::string recv_window = "5000";
    
    std::string sign_payload;
    if (method == "POST") {
        sign_payload = body.dump();
    } else {
        sign_payload = params;
    }
    
    std::string sign_str = timestamp + apiKey + recv_window + sign_payload;
    std::string signature = hmac_sha256(apiSecret, sign_str);

    httplib::Headers headers = {
        {"X-BAPI-API-KEY", apiKey},
        {"X-BAPI-SIGN", signature},
        {"X-BAPI-TIMESTAMP", timestamp},
        {"X-BAPI-RECV-WINDOW", recv_window},
        {"Content-Type", "application/json"}
    };

    httplib::Result res;
    std::string path = endpoint + (params.empty() ? "" : "?" + params);
    
    if (method == "POST") {
        res = cli.Post(path.c_str(), headers, body.dump(), "application/json");
    } else {
        res = cli.Get(path.c_str(), headers);
    }
    
    if (!res || res->status != 200) {
        return json{{"retCode", -1}, {"retMsg", "request_failed"}};
    }

    try {
        return json::parse(res->body);
    } catch (...) {
        return json{{"retCode", -1}, {"retMsg", "parse_failed"}};
    }
}

// Получить баланс USDT
double get_balance(const std::string& apiKey, const std::string& apiSecret) {
    auto result = bybit_request("GET", "/v5/account/wallet-balance", apiKey, apiSecret, 
                                json::object(), "accountType=UNIFIED");
    
    if (result.value("retCode", -1) != 0) return 0.0;
    
    try {
        auto list = result["result"]["list"];
        if (!list.empty()) {
            auto coins = list[0]["coin"];
            for (auto& coin : coins) {
                if (coin["coin"] == "USDT") {
                    return std::stod(coin.value("walletBalance", "0"));
                }
            }
        }
    } catch (...) {}
    
    return 0.0;
}

// Получить текущую позицию
json get_position(const std::string& apiKey, const std::string& apiSecret, const std::string& symbol) {
    std::string params = "category=linear&symbol=" + symbol;
    auto result = bybit_request("GET", "/v5/position/list", apiKey, apiSecret, json::object(), params);
    
    if (result.value("retCode", -1) != 0) return nullptr;
    
    try {
        auto list = result["result"]["list"];
        if (!list.empty() && list[0].value("size", "0") != "0") {
            auto pos = list[0];
            double size = std::stod(pos.value("size", "0"));
            if (size > 0) {
                return pos;
            }
        }
    } catch (...) {}
    
    return nullptr;
}

// Открыть сделку с TP/SL
bool open_trade(const std::string& apiKey, const std::string& apiSecret,
                const std::string& symbol, const std::string& side, double qty,
                double tp_percent, double sl_percent, int leverage) {
    
    // 1. Установить leverage
    json lev_body = {
        {"category", "linear"},
        {"symbol", symbol},
        {"buyLeverage", std::to_string(leverage)},
        {"sellLeverage", std::to_string(leverage)}
    };
    
    auto lev_res = bybit_request("POST", "/v5/position/set-leverage", apiKey, apiSecret, lev_body);
    int lev_code = lev_res.value("retCode", -1);
    if (lev_code != 0 && lev_code != 110043 && lev_code != 110044) {
        std::cout << "[ROBOT] Leverage failed: " << lev_res.value("retMsg", "") << std::endl;
        return false;
    }

    // 2. Открыть Market order
    json order_body = {
        {"category", "linear"},
        {"symbol", symbol},
        {"side", side},
        {"orderType", "Market"},
        {"qty", std::to_string((int)qty)},
        {"timeInForce", "GTC"},
        {"positionIdx", 0}
    };

    auto order_res = bybit_request("POST", "/v5/order/create", apiKey, apiSecret, order_body);
    
    if (order_res.value("retCode", -1) != 0) {
        std::cout << "[ROBOT] Order failed: " << order_res.value("retMsg", "") << std::endl;
        return false;
    }

    std::string orderId = order_res["result"]["orderId"];
    std::cout << "[ROBOT] Order created: " << orderId << std::endl;

    // 3. Получить реальную цену входа
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    auto pos = get_position(apiKey, apiSecret, symbol);
    if (pos.is_null()) {
        std::cout << "[ROBOT] Warning: Cannot get position for TP/SL" << std::endl;
        return true; // Ордер всё равно открыт
    }

    double avgPrice = std::stod(pos.value("avgPrice", "0"));
    if (avgPrice <= 0) {
        std::cout << "[ROBOT] Warning: avgPrice is 0" << std::endl;
        return true;
    }

    // 4. Рассчитать TP/SL от реальной цены
    double tp_price = 0.0;
    double sl_price = 0.0;
    
    if (side == "Buy") {
        tp_price = avgPrice * (1.0 + tp_percent / 100.0);
        sl_price = avgPrice * (1.0 - sl_percent / 100.0);
    } else {
        tp_price = avgPrice * (1.0 - tp_percent / 100.0);
        sl_price = avgPrice * (1.0 + sl_percent / 100.0);
    }

    // 5. Установить TP/SL
    json tpsl_body = {
        {"category", "linear"},
        {"symbol", symbol},
        {"positionIdx", 0},
        {"takeProfit", std::to_string(tp_price)},
        {"stopLoss", std::to_string(sl_price)}
    };

    auto tpsl_res = bybit_request("POST", "/v5/position/trading-stop", apiKey, apiSecret, tpsl_body);
    
    if (tpsl_res.value("retCode", -1) != 0) {
        std::cout << "[ROBOT] TP/SL failed: " << tpsl_res.value("retMsg", "") << std::endl;
        return true; // Позиция открыта, но без TP/SL
    }

    std::cout << "[ROBOT] TP/SL set: TP=" << tp_price << " SL=" << sl_price << std::endl;
    return true;
}

} // namespace robot
