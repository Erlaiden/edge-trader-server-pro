#include "agents.h"
#include "json.hpp"
#include "../utils_data.h"
#include <httplib.h>
#include <atomic>
#include <cstdlib>
#include <string>

// Подтягиваем реализацию AgentLayer в этот TU.
#include "../agents/agent_layer.cpp"

// Публичный интерфейс статуса
#include "../agents_pub.h"

using json = nlohmann::json;

namespace {

// Уникальные имена (не конфликтуют с другими TU).
inline std::string ag_qs_str(const httplib::Request& req, const char* k, const char* defv){
    return req.has_param(k) ? req.get_param_value(k) : std::string(defv);
}
inline double ag_qs_num(const httplib::Request& req, const char* k, double defv){
    if (!req.has_param(k)) return defv;
    try { return std::stod(req.get_param_value(k)); } catch (...) { return defv; }
}

// Простое состояние агента для v1.
struct AgentState {
    std::atomic<bool> running{false};
    std::string symbol{"BTCUSDT"};
    std::string interval{"15"};
    std::string mode{"idle"};
};
inline AgentState& agent_state(){ static AgentState S; return S; }

inline void json_reply(httplib::Response& res, const json& j, int code=200){
    res.status = code;
    res.set_content(j.dump(2), "application/json");
}

} // namespace

namespace etai {

// Публичный «снимок» статуса для других модулей (health_ai и т.п.)
AgentPublic get_agent_public(){
    auto& S = agent_state();
    AgentPublic p;
    p.running  = S.running.load();
    p.symbol   = S.symbol;
    p.interval = S.interval;
    p.mode     = S.mode;
    return p;
}

void setup_agents_routes(httplib::Server& svr) {
    const bool agent_enabled = std::getenv("ETAI_AGENT_ENABLE") != nullptr;

    // ===== Decision preview (как было) =====
    svr.Get("/api/agents/decision", [agent_enabled](const httplib::Request& req, httplib::Response& res) {
        json r;
        if (!agent_enabled) {
            r["ok"] = false;
            r["error"] = "Agent layer disabled (set ETAI_AGENT_ENABLE=1)";
            return json_reply(res, r);
        }

        const std::string symbol   = ag_qs_str(req, "symbol", "BTCUSDT");
        const std::string interval = ag_qs_str(req, "interval", "15");
        const double thr           = ag_qs_num(req, "thr", 0.5);

        arma::mat X; arma::vec y;
        if (!etai::load_cached_xy(symbol, interval, X, y) || X.n_rows == 0) {
            r["ok"] = false;
            r["error"] = "Failed to load cached XY or empty features";
            return json_reply(res, r, 500);
        }

        arma::rowvec feat = X.row(X.n_rows - 1);

        etai::AgentLayer layer;
        etai::AgentSummary summary = layer.decide_all(feat, thr);

        r["ok"] = true;
        r["final_signal"] = summary.final_signal;
        r["final_confidence"] = summary.final_confidence;
        r["conflict_ratio"] = summary.conflict_ratio;

        json ags = json::object();
        for (auto& kv : summary.agents) {
            ags[kv.first] = {{"signal", kv.second.signal}, {"confidence", kv.second.confidence}};
        }
        r["agents"] = ags;

        return json_reply(res, r);
    });

    // ===== run/stop/status для UI v1 =====
    svr.Get("/api/agents/run", [agent_enabled](const httplib::Request& req, httplib::Response& res){
        json r;
        if (!agent_enabled) {
            r["ok"] = false;
            r["error"] = "Agent layer disabled (set ETAI_AGENT_ENABLE=1)";
            return json_reply(res, r);
        }

        auto& S = agent_state();
        S.symbol   = ag_qs_str(req, "symbol", "BTCUSDT");
        S.interval = ag_qs_str(req, "interval", "15");
        S.mode     = ag_qs_str(req, "mode", "live");
        S.running  = true;

        r = {
            {"ok", true},
            {"running", S.running.load()},
            {"symbol", S.symbol},
            {"interval", S.interval},
            {"mode", S.mode}
        };
        return json_reply(res, r);
    });

    svr.Get("/api/agents/stop", [](const httplib::Request&, httplib::Response& res){
        auto& S = agent_state();
        S.running = false;
        S.mode = "idle";
        json r = {
            {"ok", true},
            {"running", false},
            {"symbol", S.symbol},
            {"interval", S.interval},
            {"mode", S.mode}
        };
        return json_reply(res, r);
    });

    svr.Get("/api/agents/status", [](const httplib::Request&, httplib::Response& res){
        auto& S = agent_state();
        json r = {
            {"ok", true},
            {"running", S.running.load()},
            {"symbol", S.symbol},
            {"interval", S.interval},
            {"mode", S.mode}
        };
        return json_reply(res, r);
    });
}

} // namespace etai
