#include <httplib.h>
#include <armadillo>
#include "../agents/agent_base.h"
#include "../json.hpp"

namespace etai {
using json = nlohmann::json;

// Фабричные функции определены в соответствующих src/agents/*.cpp
extern "C" AgentBase* create_agent_long();
extern "C" AgentBase* create_agent_short();
extern "C" AgentBase* create_agent_flat();
extern "C" AgentBase* create_agent_correction();
extern "C" AgentBase* create_agent_breakout();

// /api/agents/test?type=long|short|flat|correction|breakout&thr=0.5
void setup_agents_routes(httplib::Server& svr) {
    svr.Get("/api/agents/test", [](const httplib::Request& req, httplib::Response& res) {
        const std::string type = req.get_param_value("type");
        const double thr = req.has_param("thr") ? std::stod(req.get_param_value("thr")) : 0.5;

        AgentPtr agent;
        if (type == "long")            agent.reset(create_agent_long());
        else if (type == "short")      agent.reset(create_agent_short());
        else if (type == "flat")       agent.reset(create_agent_flat());
        else if (type == "correction") agent.reset(create_agent_correction());
        else if (type == "breakout")   agent.reset(create_agent_breakout());
        else {
            json err = {{"ok", false}, {"error", "unknown agent type"}};
            res.set_content(err.dump(2), "application/json");
            return;
        }

        // Заглушка фич: 28 признаков под FEAT_VERSION=9
        arma::rowvec features = arma::randn<arma::rowvec>(28);
        const int action = agent->decide(features, thr);

        json j = {
            {"ok", true},
            {"agent", agent->name()},
            {"thr", thr},
            {"confidence", agent->confidence()},
            {"decision", action}
        };
        res.set_content(j.dump(2), "application/json");
    });
}
} // namespace etai
