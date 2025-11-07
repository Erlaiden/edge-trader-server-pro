#include "json.hpp"
#include <httplib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fstream>
#include <string>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <openssl/hmac.h>

using json = nlohmann::json;

static const char* ROBOT_DIR  = "/var/lib/edge-trader/robot";
static const char* KEYS_PATH  = "/var/lib/edge-trader/robot/keys.json";
static bool ROBOT_RUNNING = false;

// --- уникальные имена, чтобы не конфликтовать с health_ai.cpp ---
static inline void rbt_ensure_dir() {
    ::mkdir(ROBOT_DIR, 0700);
    ::chmod(ROBOT_DIR, 0700);
}
static inline bool rbt_write_secure(const std::string& path, const std::string& data) {
    std::ofstream f(path, std::ios::out | std::ios::trunc);
    if (!f) return false;
    f << data;
    f.close();
    ::chmod(path.c_str(), 0600);
    return true;
}
json rbt_read_json(const std::string& path) {
    std::ifstream f(path);
    if (!f) return json::object();
    json j; try { f >> j; } catch(...) { return json::object(); }
    return j;
}

// HMAC SHA256 для Bybit
std::string hmac_sha256(const std::string& key, const std::string& data) {
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(EVP_sha256(), key.c_str(), key.length(),
         reinterpret_cast<const unsigned char*>(data.c_str()), data.length(),
         result, &len);
    std::stringstream ss;
    for (unsigned int i = 0; i < len; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)result[i];
    }
    return ss.str();
}

// Bybit API запрос
static json bybit_request(const std::string& endpoint, const std::string& apiKey, 
                          const std::string& apiSecret, const std::string& params = "") {
    httplib::SSLClient cli("api.bybit.com");
    cli.set_connection_timeout(10);
    cli.set_read_timeout(10);

    std::string timestamp = std::to_string(std::time(nullptr) * 1000);
    std::string recv_window = "5000";
    
    std::string sign_str = timestamp + apiKey + recv_window + params;
    std::string signature = hmac_sha256(apiSecret, sign_str);

    httplib::Headers headers = {
        {"X-BAPI-API-KEY", apiKey},
        {"X-BAPI-SIGN", signature},
        {"X-BAPI-TIMESTAMP", timestamp},
        {"X-BAPI-RECV-WINDOW", recv_window},
        {"Content-Type", "application/json"}
    };

    std::string path = endpoint;
    if (!params.empty() && endpoint.find('?') == std::string::npos) {
        path += "?" + params;
    }

    auto res = cli.Get(path.c_str(), headers);
    
    if (!res || res->status != 200) {
        return json{{"retCode", -1}, {"retMsg", "request_failed"}};
    }

    try {
        return json::parse(res->body);
    } catch (...) {
        return json{{"retCode", -1}, {"retMsg", "parse_failed"}};
    }
}

void register_robot_routes(httplib::Server& srv) {
    rbt_ensure_dir();

    // POST /api/robot/keys { apiKey, apiSecret, testnet? }
    srv.Post("/api/robot/keys", [&](const httplib::Request& req, httplib::Response& res){
        json out{{"ok",false}};
        try {
            if (req.body.empty()) { out["error"]="empty_body"; res.set_content(out.dump(), "application/json"); return; }
            json in = json::parse(req.body, nullptr, true);
            std::string apiKey    = in.value("apiKey",    "");
            std::string apiSecret = in.value("apiSecret", "");
            bool testnet          = in.value("testnet", false);
            if (apiKey.empty() || apiSecret.empty()) { out["error"]="missing_keys"; res.set_content(out.dump(), "application/json"); return; }
            json to_store{{"apiKey",apiKey},{"apiSecret",apiSecret},{"testnet",testnet}};
            if (!rbt_write_secure(std::string(KEYS_PATH), to_store.dump())) {
                out["error"]="write_failed"; res.set_content(out.dump(), "application/json"); return;
            }
            out["ok"]=true;
            res.set_content(out.dump(), "application/json");
        } catch(...) { out["error"]="exception"; res.set_content(out.dump(), "application/json"); }
    });

    // GET /api/robot/status
    srv.Get("/api/robot/status", [&](const httplib::Request& req, httplib::Response& res){
        json keys = rbt_read_json(std::string(KEYS_PATH));
        bool keys_present = keys.contains("apiKey") && keys.contains("apiSecret");
        json out{{"ok",true},{"running",ROBOT_RUNNING},{"keys_present",keys_present},{"testnet",keys.value("testnet",false)}};
        res.set_content(out.dump(), "application/json");
    });

    // POST /api/robot/start
    srv.Post("/api/robot/start", [&](const httplib::Request& req, httplib::Response& res){
        json keys = rbt_read_json(std::string(KEYS_PATH));
        if (!(keys.contains("apiKey") && keys.contains("apiSecret"))) {
            json out{{"ok",false},{"error","keys_missing"}}; res.set_content(out.dump(),"application/json"); return;
        }
        ROBOT_RUNNING = true;
        json out{{"ok",true},{"running",ROBOT_RUNNING}};
        res.set_content(out.dump(), "application/json");
    });

    // POST /api/robot/stop
    srv.Post("/api/robot/stop", [&](const httplib::Request& req, httplib::Response& res){
        ROBOT_RUNNING = false;
        json out{{"ok",true},{"running",ROBOT_RUNNING}};
        res.set_content(out.dump(), "application/json");
    });

    // GET /api/robot/balance
    srv.Get("/api/robot/balance", [&](const httplib::Request& req, httplib::Response& res){
        json keys = rbt_read_json(std::string(KEYS_PATH));
        if (!keys.contains("apiKey") || !keys.contains("apiSecret")) {
            json out{{"ok",false},{"error","keys_missing"}};
            res.set_content(out.dump(), "application/json");
            return;
        }

        std::string apiKey = keys["apiKey"];
        std::string apiSecret = keys["apiSecret"];

        auto result = bybit_request("/v5/account/wallet-balance", apiKey, apiSecret, "accountType=UNIFIED");
        
        if (result.value("retCode", -1) != 0) {
            json out{{"ok",false},{"balance",0},{"available",0}};
            res.set_content(out.dump(), "application/json");
            return;
        }

        double balance = 0.0;
        double available = 0.0;

        try {
            auto list = result["result"]["list"];
            if (!list.empty()) {
                auto coins = list[0]["coin"];
                for (auto& coin : coins) {
                    if (coin["coin"] == "USDT") {
                        balance = std::stod(coin.value("walletBalance", "0"));
                        available = std::stod(coin.value("availableToWithdraw", "0"));
                        break;
                    }
                }
            }
        } catch (...) {}

        json out{{"ok",true},{"balance",balance},{"available",available}};
        res.set_content(out.dump(), "application/json");
    });

    // GET /api/robot/position?symbol=BTCUSDT
    srv.Get("/api/robot/position", [&](const httplib::Request& req, httplib::Response& res){
        json keys = rbt_read_json(std::string(KEYS_PATH));
        if (!keys.contains("apiKey") || !keys.contains("apiSecret")) {
            json out{{"ok",false},{"position",nullptr}};
            res.set_content(out.dump(), "application/json");
            return;
        }

        std::string symbol = req.get_param_value("symbol");
        if (symbol.empty()) symbol = "BTCUSDT";

        std::string apiKey = keys["apiKey"];
        std::string apiSecret = keys["apiSecret"];

        std::string params = "category=linear&symbol=" + symbol;
        auto result = bybit_request("/v5/position/list", apiKey, apiSecret, params);
        
        if (result.value("retCode", -1) != 0) {
            json out{{"ok",false},{"position",nullptr}};
            res.set_content(out.dump(), "application/json");
            return;
        }

        json position = nullptr;
        try {
            auto list = result["result"]["list"];
            if (!list.empty() && list[0].value("size", "0") != "0") {
                auto pos = list[0];
                double size = std::stod(pos.value("size", "0"));
                double entryPrice = std::stod(pos.value("avgPrice", "0"));
                double markPrice = std::stod(pos.value("markPrice", "0"));
                double unrealizedPnl = std::stod(pos.value("unrealisedPnl", "0"));
                int leverage = std::stoi(pos.value("leverage", "1"));
                std::string side = pos.value("side", "");

                if (size > 0) {
                    position = json{
                        {"symbol", symbol},
                        {"side", side == "Buy" ? "Long" : "Short"},
                        {"size", size},
                        {"entryPrice", entryPrice},
                        {"markPrice", markPrice},
                        {"leverage", leverage},
                        {"pnl", unrealizedPnl}
                    };
                }
            }
        } catch (...) {}

        json out{{"ok",true},{"position",position}};
        res.set_content(out.dump(), "application/json");
    });

    // GET /api/robot/pnl
    srv.Get("/api/robot/pnl", [&](const httplib::Request& req, httplib::Response& res){
        // Временно возвращаем моковые данные
        // TODO: реализовать подсчёт PnL из истории сделок
        json out{{"ok",true},{"today",0.0},{"total",0.0},{"unrealized",0.0}};
        res.set_content(out.dump(), "application/json");
    });
}
