#include "agents.h"
#include "json.hpp"
#include "../utils_data.h"
#include <cstdlib>
#include <string>

// Подтягиваем реализацию AgentLayer в этот TU,
// чтобы не менять CMake прямо сейчас.
#include "../agents/agent_layer.cpp"

using json = nlohmann::json;

namespace {

inline std::string get_qs_or_default(const httplib::Request& req,
                                     const char* key,
                                     const char* defval) {
    return req.has_param(key) ? req.get_param_value(key) : std::string(defval);
}
inline double get_qs_double_or_default(const httplib::Request& req,
                                       const char* key,
                                       double defval) {
    if (!req.has_param(key)) return defval;
    try { return std::stod(req.get_param_value(key)); } catch (...) { return defval; }
}

} // namespace

namespace etai {

void setup_agents_routes(httplib::Server& svr) {
    const bool agent_enabled = std::getenv("ETAI_AGENT_ENABLE") != nullptr;

    svr.Get("/api/agents/decision", [agent_enabled](const httplib::Request& req, httplib::Response& res) {
        json reply;

        if (!agent_enabled) {
            reply["ok"] = false;
            reply["error"] = "Agent layer disabled (set ETAI_AGENT_ENABLE=1)";
            res.status = 200;
            res.set_content(reply.dump(2), "application/json");
            return;
        }

        const std::string symbol   = get_qs_or_default(req, "symbol", "BTCUSDT");
        const std::string interval = get_qs_or_default(req, "interval", "15");
        const double thr           = get_qs_double_or_default(req, "thr", 0.5);

        arma::mat X; arma::vec y;
        if (!etai::load_cached_xy(symbol, interval, X, y) || X.n_rows == 0) {
            reply["ok"] = false;
            reply["error"] = "Failed to load cached XY or empty features";
            res.status = 500;
            res.set_content(reply.dump(2), "application/json");
            return;
        }

        arma::rowvec feat = X.row(X.n_rows - 1);

        etai::AgentLayer layer;
        etai::AgentSummary summary = layer.decide_all(feat, thr);

        reply["ok"] = true;
        reply["final_signal"] = summary.final_signal;
        reply["final_confidence"] = summary.final_confidence;
        reply["conflict_ratio"] = summary.conflict_ratio;

        json ags = json::object();
        for (auto& kv : summary.agents) {
            ags[kv.first] = {{"signal", kv.second.signal}, {"confidence", kv.second.confidence}};
        }
        reply["agents"] = ags;

        res.status = 200;
        res.set_content(reply.dump(2), "application/json");
    });
}

} // namespace etai
