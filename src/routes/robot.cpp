#include "json.hpp"
#include <httplib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fstream>
#include <string>

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
static inline json rbt_read_json(const std::string& path) {
    std::ifstream f(path);
    if (!f) return json::object();
    json j; try { f >> j; } catch(...) { return json::object(); }
    return j;
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

    // POST /api/robot/start { ... }
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
}
