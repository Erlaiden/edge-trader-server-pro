#include <httplib.h>
#include <armadillo>
#include <sys/stat.h>
#include <fstream>
#include <ctime>

#include "../agents/agent_base.h"
#include "../json.hpp"
#include "../utils_data.h"      // etai::load_raw_ohlcv(...)
#include "../infer_policy.h"    // infer_with_policy(...)
#include "../server_accessors.h"// atoms & model

namespace etai {
using json = nlohmann::json;

// Фабрики простых ботов (оставляем тестовый эндпоинт)
extern "C" AgentBase* create_agent_long();
extern "C" AgentBase* create_agent_short();
extern "C" AgentBase* create_agent_flat();
extern "C" AgentBase* create_agent_correction();
extern "C" AgentBase* create_agent_breakout();

// mkdir -p style (best-effort)
static inline void ensure_dir_p(const char* /*p*/){
  ::mkdir("cache", 0755);
  ::mkdir("cache/logs", 0755);
}

static inline int sign_from_string(const std::string& sig){
  if(sig=="LONG")  return  1;
  if(sig=="SHORT") return -1;
  return 0;
}

// /api/agents/test?type=long|short|flat|correction|breakout&thr=0.5
void setup_agents_routes(httplib::Server& svr) {

  // Тестовый эндпоинт — случайные фичи (для экспресс-проверки фабрик)
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

    arma::rowvec features = arma::randn<arma::rowvec>(28); // заглушечные фичи
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

  // Реальный запуск агента на текущей модели и данных с диска:
  // /api/agents/run?type=breakout&symbol=BTCUSDT&interval=15&thr=0.4
  svr.Get("/api/agents/run", [](const httplib::Request& req, httplib::Response& res) {
    const std::string type     = req.has_param("type")     ? req.get_param_value("type")     : "breakout";
    const std::string symbol   = req.has_param("symbol")   ? req.get_param_value("symbol")   : "BTCUSDT";
    const std::string interval = req.has_param("interval") ? req.get_param_value("interval") : "15";
    const double thr           = req.has_param("thr")      ? std::stod(req.get_param_value("thr")) : etai::get_model_thr();

    // 1) Данные
    arma::mat raw15;
    if(!etai::load_raw_ohlcv(symbol, interval, raw15) || raw15.n_rows < 60 || raw15.n_cols < 6){
      json out{{"ok", false}, {"error","not_enough_data"}, {"rows",(int)raw15.n_rows}, {"cols",(int)raw15.n_cols}};
      res.set_content(out.dump(2), "application/json");
      return;
    }

    // 2) Модель (из атомика)
    nlohmann::json model = etai::get_current_model();
    if(!model.is_object() || !model.contains("policy")){
      json out{{"ok", false}, {"error","no_model_policy"}};
      res.set_content(out.dump(2), "application/json");
      return;
    }

    // 3) Инференс по политике
    nlohmann::json inf = infer_with_policy(raw15, model);
    if(!inf.value("ok", false)){
      json out{{"ok", false}, {"error","infer_failed"}, {"details", inf}};
      res.set_content(out.dump(2), "application/json");
      return;
    }

    // 4) Телеметрия последнего инференса -> атомики
    const std::string sig = inf.value("signal", std::string("NEUTRAL"));
    const double score    = inf.value("score", 0.0);
    const double sigma    = inf.value("sigma", 0.0);
    const int sgn         = sign_from_string(sig);

    etai::set_last_infer_score(score);
    etai::set_last_infer_sigma(sigma);
    etai::set_last_infer_signal(sgn);

    // 5) Лог в TSV (best-effort)
    ensure_dir_p("cache/logs");
    {
      std::ofstream f("cache/logs/agents_runs.tsv", std::ios::app);
      if(f.good()){
        const std::time_t t = std::time(nullptr);
        f << t << '\t'
          << symbol << '\t' << interval << '\t' << type << '\t'
          << thr << '\t' << sig << '\t' << score << '\t' << sigma << '\n';
      }
    }

    // 6) Ответ
    json out{
      {"ok", true},
      {"agent", type},
      {"symbol", symbol},
      {"interval", interval},
      {"thr", thr},
      {"infer", {
        {"signal", sig},
        {"score", score},
        {"sigma", sigma},
        {"vol_threshold", inf.value("vol_threshold", 0.0)}
      }},
      {"decision", sgn},
      {"confidence", std::fabs(score)}
    };
    res.set_content(out.dump(2), "application/json");
  });
}

} // namespace etai
