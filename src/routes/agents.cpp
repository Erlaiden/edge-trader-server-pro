#include "../agents.h"
#include "json.hpp"
#include "../utils_data.h"
#include <httplib.h>
#include <atomic>
#include <cstdlib>
#include <string>
#include <armadillo>

// Подтягиваем реализацию AgentLayer в этот TU, чтобы не трогать CMake.
#include "../agents/agent_layer.cpp"

using json = nlohmann::json;

namespace {
// Уникальные хелперы (чтобы не конфликтовать с другими TU)
inline std::string ag_qs_str(const httplib::Request& req, const char* k, const char* defv){
    return req.has_param(k) ? req.get_param_value(k) : std::string(defv);
}
inline double ag_qs_num(const httplib::Request& req, const char* k, double defv){
    if (!req.has_param(k)) return defv;
    try { return std::stod(req.get_param_value(k)); } catch (...) { return defv; }
}

// Простое состояние агента (v1)
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

// Публичный статус для health_ai
nlohmann::json get_agent_public(){
    auto& S = agent_state();
    json j;
    j["running"]  = S.running.load();
    j["mode"]     = S.mode;
    j["symbol"]   = S.symbol;
    j["interval"] = S.interval;
    return j;
}

void setup_agents_routes(httplib::Server& svr) {
    const bool agent_enabled = std::getenv("ETAI_AGENT_ENABLE") != nullptr;

    // Decision preview
    svr.Get("/api/agents/decision", [agent_enabled](const httplib::Request& req, httplib::Response& res) {
        json r;
        if (!agent_enabled) {
            r["ok"] = false;
            r["error"] = "Agent layer disabled (set ETAI_AGENT_ENABLE=1)";
            return json_reply(res, r);
        }

        const std::string symbol   = ag_qs_str(req, "symbol", "BTCUSDT");
        const std::string interval = ag_qs_str(req, "interval", "15");
        const double      thr      = ag_qs_num(req, "thr", 0.5);

        arma::mat X, y;
        if (!etai::load_cached_xy(symbol, interval, X, y) || X.n_rows == 0) {
            r["ok"] = false;
            r["error"] = "Failed to load cached XY or empty features";
            return json_reply(res, r, 500);
        }

        arma::rowvec feat = X.row(X.n_rows - 1);
        etai::AgentLayer layer;
        etai::AgentSummary summary = layer.decide_all(feat, thr);

        r["ok"] = true;
        r["final_signal"]     = summary.final_signal;
        r["final_confidence"] = summary.final_confidence;
        r["conflict_ratio"]   = summary.conflict_ratio;

        json ags = json::object();
        for (auto& kv : summary.agents) {
            ags[kv.first] = {{"signal", kv.second.signal}, {"confidence", kv.second.confidence}};
        }
        r["agents"] = ags;
        return json_reply(res, r);
    });

    // run/stop/status (v1) с валидацией данных
    svr.Get("/api/agents/run", [agent_enabled](const httplib::Request& req, httplib::Response& res){
        json r;
        if (!agent_enabled) {
            r["ok"] = false;
            r["error"] = "Agent layer disabled (set ETAI_AGENT_ENABLE=1)";
            return json_reply(res, r);
        }

        const std::string symbol   = ag_qs_str(req, "symbol", "BTCUSDT");
        const std::string interval = ag_qs_str(req, "interval", "15");
        const std::string mode     = ag_qs_str(req, "mode", "live");

        json dh = etai::data_health_report(symbol, interval);
        const bool data_ok = dh.value("ok", false);

        arma::mat X, Y;
        bool xy_ok = false;
        if (data_ok) {
            xy_ok = etai::load_cached_xy(symbol, interval, X, Y) && X.n_rows > 0;
        }

        if (!data_ok || !xy_ok) {
            r["ok"] = false;
            r["error"] = "Data not ready for the requested symbol/interval";
            r["symbol"] = symbol;
            r["interval"] = interval;
            r["agents"] = { {"running", false}, {"mode", "idle"} };
            r["data_health"] = dh;
            r["xy_ready"]    = xy_ok;
            return json_reply(res, r, 409);
        }

        auto& S = agent_state();
        S.symbol   = symbol;
        S.interval = interval;
        S.mode     = mode;
        S.running  = true;

        r["ok"]       = true;
        r["running"]  = true;
        r["symbol"]   = S.symbol;
        r["interval"] = S.interval;
        r["mode"]     = S.mode;
        r["data_health"] = {
            {"ok", true},
            {"clean_rows", dh["clean"].value("rows", 0)},
            {"clean_cols", dh["clean"].value("cols", 0)},
            {"raw_rows",   dh["raw"].value("rows", 0)},
            {"raw_cols",   dh["raw"].value("cols", 0)}
        };
        r["features"] = { {"rows", (unsigned)X.n_rows}, {"cols", (unsigned)X.n_cols} };
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
