#include <httplib.h>
#include <armadillo>
#include <ctime>
#include <string>
#include "../agents/agent_base.h"
#include "../json.hpp"
#include "../rt_metrics.h"
#include "../utils_data.h"
#include "../infer_policy.h"
#include "../server_accessors.h"

using json = nlohmann::json;

namespace etai {

// Фабричные функции агентов (реализация в src/agents/*.cpp)
extern "C" AgentBase* create_agent_long();
extern "C" AgentBase* create_agent_short();
extern "C" AgentBase* create_agent_flat();
extern "C" AgentBase* create_agent_correction();
extern "C" AgentBase* create_agent_breakout();

// Локальная утилита: обновление атомиков метрик последнего инференса
static inline void update_infer_metrics_from(const json& infer) {
    try {
        const double score = infer.value("score", 0.0);
        const double sigma = infer.value("sigma", 0.0);
        const std::string sig = infer.value("signal", "NEUTRAL");

        long long sigv = 0;
        if (sig == "LONG")      sigv = 1;
        else if (sig == "SHORT") sigv = -1;

        LAST_INFER_SCORE.store(score, std::memory_order_relaxed);
        LAST_INFER_SIGMA.store(sigma, std::memory_order_relaxed);
        LAST_INFER_SIGNAL.store(sigv, std::memory_order_relaxed);
        LAST_INFER_TS.store((long long)std::time(nullptr) * 1000, std::memory_order_relaxed);

        if      (sigv ==  1) INFER_SIG_LONG.fetch_add(1, std::memory_order_relaxed);
        else if (sigv == -1) INFER_SIG_SHORT.fetch_add(1, std::memory_order_relaxed);
        else                 INFER_SIG_NEUTRAL.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
        // не даём упасть роуту из-за телеметрии
    }
}

// /api/agents/test — заглушка на случай быстрых проверок фабрик
// GET /api/agents/test?type=long|short|flat|correction|breakout&thr=0.5
void setup_agents_routes(httplib::Server& svr) {
    svr.Get("/api/agents/test", [](const httplib::Request& req, httplib::Response& res) {
        REQ_INFER.fetch_add(1, std::memory_order_relaxed);

        const std::string type = req.has_param("type") ? req.get_param_value("type") : "breakout";
        const double thr = req.has_param("thr") ? std::stod(req.get_param_value("thr")) : 0.5;

        AgentPtr agent;
        if      (type == "long")       agent.reset(create_agent_long());
        else if (type == "short")      agent.reset(create_agent_short());
        else if (type == "flat")       agent.reset(create_agent_flat());
        else if (type == "correction") agent.reset(create_agent_correction());
        else if (type == "breakout")   agent.reset(create_agent_breakout());
        else {
            res.set_content(json{{"ok", false}, {"error", "unknown agent type"}}.dump(2), "application/json");
            return;
        }

        arma::rowvec features = arma::randn<arma::rowvec>(28);
        const int action = agent->decide(features, thr);

        json j{
            {"ok", true},
            {"agent", agent->name()},
            {"thr", thr},
            {"confidence", agent->confidence()},
            {"decision", action}
        };
        res.set_content(j.dump(2), "application/json");
    });

    // /api/agents/run — реальный инференс по данным + обновление метрик
    // GET /api/agents/run?type=breakout&symbol=BTCUSDT&interval=15&thr=0.4
    svr.Get("/api/agents/run", [](const httplib::Request& req, httplib::Response& res) {
        REQ_INFER.fetch_add(1, std::memory_order_relaxed);

        const std::string type     = req.has_param("type")     ? req.get_param_value("type")     : "breakout";
        const std::string symbol   = req.has_param("symbol")   ? req.get_param_value("symbol")   : "BTCUSDT";
        const std::string interval = req.has_param("interval") ? req.get_param_value("interval") : "15";
        const double thr = req.has_param("thr") ? std::stod(req.get_param_value("thr"))
                                                : etai::get_model_thr();

        arma::mat raw15;
        if (!etai::load_raw_ohlcv(symbol, interval, raw15)) {
            res.set_content(json{{"ok", false}, {"error", "no_data"}}.dump(2), "application/json");
            return;
        }

        const json model = etai::get_current_model();
        json infer = etai::infer_with_policy(raw15, model);
        if (!infer.value("ok", false)) {
            res.set_content(json{
                {"ok", false},
                {"error", "infer_failed"},
                {"details", infer}
            }.dump(2), "application/json");
            return;
        }

        // критично: обновляем атомики метрик
        update_infer_metrics_from(infer);

        json out{
            {"ok", true},
            {"agent", type},
            {"thr", thr},
            {"score", infer.value("score", 0.0)},
            {"signal", infer.value("signal", "")},
            {"sigma", infer.value("sigma", 0.0)},
            {"interval", interval},
            {"symbol", symbol}
        };
        res.set_content(out.dump(2), "application/json");
    });
}

} // namespace etai
