#include "json.hpp"
#include <httplib.h>
#include <sys/stat.h>
#include <fstream>
#include <string>

using json = nlohmann::json;

static const char* ROBOT_DIR  = "/var/lib/edge-trader/robot";
static const char* KEYS_PATH  = "/var/lib/edge-trader/robot/keys.json";
static const char* CONFIG_PATH = "/var/lib/edge-trader/robot/config.json";

namespace robot {
    struct RobotConfig;
    extern bool start(const std::string& apiKey, const std::string& apiSecret, const RobotConfig& cfg);
    extern void stop();
    extern bool is_running();
    extern RobotConfig get_config();
    extern double get_balance(const std::string& apiKey, const std::string& apiSecret);
    extern json get_position(const std::string& apiKey, const std::string& apiSecret, const std::string& symbol);
}

static inline void ensure_dir() {
    ::mkdir(ROBOT_DIR, 0700);
    ::chmod(ROBOT_DIR, 0700);
}

static inline bool write_secure(const std::string& path, const std::string& data) {
    std::ofstream f(path, std::ios::out | std::ios::trunc);
    if (!f) return false;
    f << data;
    f.close();
    ::chmod(path.c_str(), 0600);
    return true;
}

static json read_json(const std::string& path) {
    std::ifstream f(path);
    if (!f) return json::object();
    json j; 
    try { f >> j; } catch(...) { return json::object(); }
    return j;
}

void register_robot_routes(httplib::Server& srv) {
    ensure_dir();

    srv.Post("/api/robot/keys", [&](const httplib::Request& req, httplib::Response& res){
        json out{{"ok",false}};
        try {
            json in = json::parse(req.body);
            std::string apiKey = in.value("apiKey", "");
            std::string apiSecret = in.value("apiSecret", "");
            bool testnet = in.value("testnet", false);
            
            if (apiKey.empty() || apiSecret.empty()) {
                out["error"] = "missing_keys";
                res.set_content(out.dump(), "application/json");
                return;
            }
            
            json to_store{{"apiKey",apiKey},{"apiSecret",apiSecret},{"testnet",testnet}};
            if (!write_secure(std::string(KEYS_PATH), to_store.dump())) {
                out["error"] = "write_failed";
                res.set_content(out.dump(), "application/json");
                return;
            }
            
            out["ok"] = true;
            res.set_content(out.dump(), "application/json");
        } catch(...) {
            out["error"] = "exception";
            res.set_content(out.dump(), "application/json");
        }
    });

    srv.Get("/api/robot/status", [&](const httplib::Request& req, httplib::Response& res){
        json keys = read_json(std::string(KEYS_PATH));
        bool keys_present = keys.contains("apiKey") && keys.contains("apiSecret");
        bool running = robot::is_running();
        
        json out{
            {"ok", true},
            {"running", running},
            {"keys_present", keys_present},
            {"testnet", keys.value("testnet", false)}
        };
        
        res.set_content(out.dump(), "application/json");
    });

    srv.Post("/api/robot/start", [&](const httplib::Request& req, httplib::Response& res){
        json out{{"ok",false}};
        
        json keys = read_json(std::string(KEYS_PATH));
        if (!keys.contains("apiKey") || !keys.contains("apiSecret")) {
            out["error"] = "keys_missing";
            res.set_content(out.dump(), "application/json");
            return;
        }

        json cfg_json = read_json(std::string(CONFIG_PATH));
        
        robot::RobotConfig cfg;
        cfg.symbol = cfg_json.value("symbol", "AIAUSDT");
        cfg.leverage = cfg_json.value("leverage", 10);
        cfg.balance_percent = cfg_json.value("balancePercent", 90.0);
        cfg.tp_percent = cfg_json.value("tpPercent", 2.0);
        cfg.sl_percent = cfg_json.value("slPercent", 1.0);
        cfg.min_confidence = cfg_json.value("minConfidence", 60.0);
        cfg.check_interval_sec = cfg_json.value("checkInterval", 60);

        bool success = robot::start(keys["apiKey"], keys["apiSecret"], cfg);
        
        out["ok"] = success;
        out["running"] = robot::is_running();
        res.set_content(out.dump(), "application/json");
    });

    srv.Post("/api/robot/stop", [&](const httplib::Request& req, httplib::Response& res){
        robot::stop();
        json out{{"ok", true}, {"running", false}};
        res.set_content(out.dump(), "application/json");
    });

    srv.Get("/api/robot/balance", [&](const httplib::Request& req, httplib::Response& res){
        json keys = read_json(std::string(KEYS_PATH));
        if (!keys.contains("apiKey") || !keys.contains("apiSecret")) {
            json out{{"ok",false},{"balance",0},{"available",0}};
            res.set_content(out.dump(), "application/json");
            return;
        }

        double balance = robot::get_balance(keys["apiKey"], keys["apiSecret"]);
        json out{{"ok",true},{"balance",balance},{"available",balance}};
        res.set_content(out.dump(), "application/json");
    });

    srv.Get("/api/robot/position", [&](const httplib::Request& req, httplib::Response& res){
        json keys = read_json(std::string(KEYS_PATH));
        if (!keys.contains("apiKey") || !keys.contains("apiSecret")) {
            json out{{"ok",false},{"position",nullptr}};
            res.set_content(out.dump(), "application/json");
            return;
        }

        std::string symbol = req.get_param_value("symbol");
        if (symbol.empty()) symbol = "AIAUSDT";

        auto pos = robot::get_position(keys["apiKey"], keys["apiSecret"], symbol);
        
        json position = nullptr;
        if (!pos.is_null()) {
            try {
                double size = std::stod(pos.value("size", "0"));
                double entryPrice = std::stod(pos.value("avgPrice", "0"));
                double markPrice = std::stod(pos.value("markPrice", "0"));
                double pnl = std::stod(pos.value("unrealisedPnl", "0"));
                int leverage = std::stoi(pos.value("leverage", "1"));
                std::string side = pos.value("side", "");

                position = json{
                    {"symbol", symbol},
                    {"side", side == "Buy" ? "Long" : "Short"},
                    {"size", size},
                    {"entryPrice", entryPrice},
                    {"markPrice", markPrice},
                    {"leverage", leverage},
                    {"pnl", pnl}
                };
            } catch(...) {}
        }

        json out{{"ok",true},{"position",position}};
        res.set_content(out.dump(), "application/json");
    });

    srv.Get("/api/robot/pnl", [&](const httplib::Request& req, httplib::Response& res){
        json out{{"ok",true},{"today",0.0},{"total",0.0},{"unrealized",0.0}};
        res.set_content(out.dump(), "application/json");
    });

    srv.Post("/api/robot/config", [&](const httplib::Request& req, httplib::Response& res){
        json out{{"ok",false}};
        try {
            json in = json::parse(req.body);
            if (!write_secure(std::string(CONFIG_PATH), in.dump())) {
                out["error"] = "write_failed";
                res.set_content(out.dump(), "application/json");
                return;
            }
            out["ok"] = true;
            res.set_content(out.dump(), "application/json");
        } catch(...) {
            out["error"] = "exception";
            res.set_content(out.dump(), "application/json");
        }
    });

    srv.Get("/api/robot/config", [&](const httplib::Request& req, httplib::Response& res){
        json cfg = read_json(std::string(CONFIG_PATH));
        if (cfg.empty()) {
            cfg = json{
                {"symbol", "AIAUSDT"},
                {"leverage", 10},
                {"balancePercent", 90},
                {"tpPercent", 2.0},
                {"slPercent", 1.0},
                {"minConfidence", 60.0}
            };
        }
        json out{{"ok", true}, {"config", cfg}};
        res.set_content(out.dump(), "application/json");
    });
}
