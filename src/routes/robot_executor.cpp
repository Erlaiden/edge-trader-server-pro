#include "json.hpp"
#include <httplib.h>
#include <string>
#include <sstream>
#include <fstream>

using json = nlohmann::json;

extern std::string hmac_sha256(const std::string& key, const std::string& data);
extern json rbt_read_json(const std::string& path);
extern const char* KEYS_PATH;
extern bool ROBOT_RUNNING;

static json bybit_post(const std::string& endpoint, const std::string& apiKey, 
                       const std::string& apiSecret, const json& body) {
    httplib::SSLClient cli("api.bybit.com");
    cli.set_connection_timeout(10);
    cli.set_read_timeout(10);

    std::string timestamp = std::to_string(std::time(nullptr) * 1000);
    std::string recv_window = "5000";
    std::string body_str = body.dump();
    
    std::string sign_str = timestamp + apiKey + recv_window + body_str;
    std::string signature = hmac_sha256(apiSecret, sign_str);

    httplib::Headers headers = {
        {"X-BAPI-API-KEY", apiKey},
        {"X-BAPI-SIGN", signature},
        {"X-BAPI-TIMESTAMP", timestamp},
        {"X-BAPI-RECV-WINDOW", recv_window},
        {"Content-Type", "application/json"}
    };

    auto res = cli.Post(endpoint.c_str(), headers, body_str, "application/json");
    
    if (!res || res->status != 200) {
        return json{{"retCode", -1}, {"retMsg", "request_failed"}};
    }

    try {
        return json::parse(res->body);
    } catch (...) {
        return json{{"retCode", -1}, {"retMsg", "parse_failed"}};
    }
}

void register_robot_trading(httplib::Server& srv) {
    srv.Post("/api/robot/trade/open", [&](const httplib::Request& req, httplib::Response& res){
        json out{{"ok", false}};
        
        try {
            json keys = rbt_read_json(std::string(KEYS_PATH));
            if (!keys.contains("apiKey") || !keys.contains("apiSecret")) {
                out["error"] = "keys_missing";
                out["message"] = "Bybit ключи не настроены";
                res.set_content(out.dump(), "application/json");
                return;
            }

            if (!ROBOT_RUNNING) {
                out["error"] = "robot_stopped";
                out["message"] = "Робот остановлен";
                res.set_content(out.dump(), "application/json");
                return;
            }

            json in = json::parse(req.body);
            std::string symbol = in.value("symbol", "");
            std::string side = in.value("side", "");
            double qty = in.value("qty", 0.0);
            double tp_price = in.value("tp_price", 0.0);
            double sl_price = in.value("sl_price", 0.0);
            int leverage = in.value("leverage", 5);

            if (symbol.empty() || side.empty() || qty <= 0) {
                out["error"] = "invalid_params";
                out["message"] = "Неверные параметры";
                res.set_content(out.dump(), "application/json");
                return;
            }

            std::string apiKey = keys["apiKey"];
            std::string apiSecret = keys["apiSecret"];

            // Устанавливаем leverage (игнорируем ошибку если уже установлено)
            json leverage_body = {
                {"category", "linear"},
                {"symbol", symbol},
                {"buyLeverage", std::to_string(leverage)},
                {"sellLeverage", std::to_string(leverage)}
            };
            
            auto lev_res = bybit_post("/v5/position/set-leverage", apiKey, apiSecret, leverage_body);
            // Игнорируем коды 110043 (leverage not modified) и 110044 (leverage invalid)
            int lev_code = lev_res.value("retCode", -1);
            if (lev_code != 0 && lev_code != 110043 && lev_code != 110044) {
                out["error"] = "leverage_failed";
                out["message"] = "Не удалось установить плечо: " + lev_res.value("retMsg", "unknown");
                res.set_content(out.dump(), "application/json");
                return;
            }

            // Открываем позицию
            json order_body = {
                {"category", "linear"},
                {"symbol", symbol},
                {"side", side},
                {"orderType", "Market"},
                {"qty", std::to_string(qty)},
                {"timeInForce", "GTC"},
                {"positionIdx", 0}
            };

            if (tp_price > 0) {
                order_body["takeProfit"] = std::to_string(tp_price);
            }
            if (sl_price > 0) {
                order_body["stopLoss"] = std::to_string(sl_price);
            }

            auto order_res = bybit_post("/v5/order/create", apiKey, apiSecret, order_body);
            
            if (order_res.value("retCode", -1) != 0) {
                out["error"] = "order_failed";
                out["message"] = "Не удалось открыть сделку: " + order_res.value("retMsg", "unknown");
                res.set_content(out.dump(), "application/json");
                return;
            }

            out["ok"] = true;
            out["orderId"] = order_res["result"]["orderId"];
            out["message"] = "Сделка открыта";
            res.set_content(out.dump(), "application/json");

        } catch (std::exception& e) {
            out["error"] = "exception";
            out["message"] = std::string("Ошибка: ") + e.what();
            res.set_content(out.dump(), "application/json");
        }
    });
}
