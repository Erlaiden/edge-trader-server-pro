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

bool update_stop_loss(const std::string& apiKey, const std::string& apiSecret,
                      const std::string& symbol, double new_sl_price) {

    std::ostringstream sl_stream;
    sl_stream << std::fixed << std::setprecision(6) << new_sl_price;
    std::string sl_str = sl_stream.str();

    sl_str.erase(sl_str.find_last_not_of('0') + 1, std::string::npos);
    if (sl_str.back() == '.') sl_str.pop_back();

    json tpsl_body = {
        {"category", "linear"},
        {"symbol", symbol},
        {"positionIdx", 0},
        {"stopLoss", sl_str}
    };

    auto res = bybit_request("POST", "/v5/position/trading-stop", apiKey, apiSecret, tpsl_body);

    if (res.value("retCode", -1) != 0) {
        std::cout << "[BREAKEVEN] ❌ Failed to update SL: " << res.value("retMsg", "") << std::endl;
        return false;
    }

    std::cout << "[BREAKEVEN] ✅ SL updated to: $" << sl_str << std::endl;
    return true;
}

bool open_trade(const std::string& apiKey, const std::string& apiSecret,
                const std::string& symbol, const std::string& side, double qty,
                double tp_percent, double sl_percent, int leverage) {

    std::cout << "[TRADE] Opening position..." << std::endl;
    std::cout << "[TRADE] Params: " << symbol << " " << side << " qty=" << qty
              << " TP=" << tp_percent << "% SL=" << sl_percent << "% lev=" << leverage << std::endl;

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
        std::cout << "[TRADE] Leverage failed: " << lev_res.value("retMsg", "") << std::endl;
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
        std::cout << "[TRADE] Order failed: " << order_res.value("retMsg", "") << std::endl;
        return false;
    }

    std::string orderId = order_res["result"]["orderId"];
    std::cout << "[TRADE] Order created: " << orderId << ", waiting for fill..." << std::endl;

    // 3. Подождать исполнения и получить РЕАЛЬНУЮ entry_price
    std::this_thread::sleep_for(std::chrono::seconds(3));

    auto position = get_position(apiKey, apiSecret, symbol);
    if (position.is_null()) {
        std::cout << "[TRADE] ❌ CRITICAL: Position not found after order!" << std::endl;
        return false;
    }

    double real_entry_price = std::stod(position.value("avgPrice", "0"));
    if (real_entry_price <= 0) {
        std::cout << "[TRADE] ❌ CRITICAL: Invalid entry price!" << std::endl;
        return false;
    }

    std::cout << "[TRADE] ✅ Position opened at: $" << real_entry_price << std::endl;

    // 4. Рассчитать TP/SL на основе РЕАЛЬНОЙ entry_price
    double tp_price = 0.0;
    double sl_price = 0.0;

    if (side == "Buy") {
        tp_price = real_entry_price * (1.0 + tp_percent / 100.0);
        sl_price = real_entry_price * (1.0 - sl_percent / 100.0);
    } else {
        tp_price = real_entry_price * (1.0 - tp_percent / 100.0);
        sl_price = real_entry_price * (1.0 + sl_percent / 100.0);
    }

    std::cout << "[TRADE] Calculated TP/SL: TP=$" << tp_price << " SL=$" << sl_price << std::endl;

    // 5. Установить TP/SL
    std::ostringstream tp_stream, sl_stream;
    tp_stream << std::fixed << std::setprecision(6) << tp_price;
    sl_stream << std::fixed << std::setprecision(6) << sl_price;

    std::string tp_str = tp_stream.str();
    std::string sl_str = sl_stream.str();

    tp_str.erase(tp_str.find_last_not_of('0') + 1, std::string::npos);
    sl_str.erase(sl_str.find_last_not_of('0') + 1, std::string::npos);
    if (tp_str.back() == '.') tp_str.pop_back();
    if (sl_str.back() == '.') sl_str.pop_back();

    json tpsl_body = {
        {"category", "linear"},
        {"symbol", symbol},
        {"positionIdx", 0},
        {"takeProfit", tp_str},
        {"stopLoss", sl_str}
    };

    auto tpsl_res = bybit_request("POST", "/v5/position/trading-stop", apiKey, apiSecret, tpsl_body);

    if (tpsl_res.value("retCode", -1) != 0) {
        std::cout << "[TRADE] ❌ CRITICAL: TP/SL failed: " << tpsl_res.value("retMsg", "") << std::endl;
        std::cout << "[TRADE] ⚠️  Position opened WITHOUT protection!" << std::endl;
        std::cout << "[TRADE] 🔴 MANUAL ACTION REQUIRED - Close position manually!" << std::endl;
        return false;
    }

    std::cout << "[TRADE] ✅ TP/SL set successfully: TP=" << tp_str << " SL=" << sl_str << std::endl;
    return true;
}

} // namespace robot
