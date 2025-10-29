#include "train.h"
#include "json.hpp"
#include "http_helpers.h"
#include "train_logic.h"
#include "rt_metrics.h"
#include <ctime>

using json = nlohmann::json;

void register_train_routes(httplib::Server& srv) {
  auto train_pro_handler = [](const httplib::Request& req, httplib::Response& res){
    REQ_TRAIN.fetch_add(1, std::memory_order_relaxed);
    std::string symbol   = qp(req, "symbol", "BTCUSDT");
    std::string interval = qp(req, "interval", "15");
    int episodes = 40;
    double tp = 0.008, sl = 0.0032; int ma_len = 12;
    if (!qp(req,"episodes").empty()) episodes = std::atoi(qp(req,"episodes").c_str());
    if (!qp(req,"tp").empty())       tp = std::atof(qp(req,"tp").c_str());
    if (!qp(req,"sl").empty())       sl = std::atof(qp(req,"sl").c_str());
    if (!qp(req,"ma").empty())       ma_len = std::atoi(qp(req,"ma").c_str());

    json out = run_train_pro_and_save(symbol, interval, episodes, tp, sl, ma_len);
    res.set_content(out.dump(2), "application/json");
  };

  srv.Get ("/api/train",     train_pro_handler);
  srv.Post("/api/train",     train_pro_handler);
  srv.Get ("/api/train/pro", train_pro_handler);

  auto train_summary_handler = [](const httplib::Request& req, httplib::Response& res){
    REQ_TRAIN.fetch_add(1, std::memory_order_relaxed);
    std::string symbol   = qp(req, "symbol", "BTCUSDT");
    std::string interval = qp(req, "interval", "15");
    int episodes = 40;
    double tp = 0.008, sl = 0.0032; int ma_len = 12;
    if (!qp(req,"episodes").empty()) episodes = std::atoi(qp(req,"episodes").c_str());
    if (!qp(req,"tp").empty())       tp = std::atof(qp(req,"tp").c_str());
    if (!qp(req,"sl").empty())       sl = std::atof(qp(req,"sl").c_str());
    if (!qp(req,"ma").empty())       ma_len = std::atoi(qp(req,"ma").c_str());

    json full = run_train_pro_and_save(symbol, interval, episodes, tp, sl, ma_len);
    json m = full.value("metrics", json::object());
    json is = m.contains("is_summary") ? m["is_summary"] : (m.contains("is") ? m["is"] : json::object());
    json oos= m.contains("oos_summary")? m["oos_summary"]: (m.contains("oos")? m["oos"]: json::object());

    json out{
      {"ok", full.value("ok", false)},
      {"symbol", symbol},
      {"interval", interval},
      {"cv_folds", m.value("cv_folds", (unsigned long long)0)},
      {"cv_effective_folds", m.value("cv_effective_folds", (unsigned long long)0)},
      {"is", json{
        {"sharpe",     is.value("sharpe", 0.0)},
        {"expectancy", is.value("expectancy", 0.0)}
      }},
      {"oos", json{
        {"sharpe",      oos.value("sharpe", 0.0)},
        {"expectancy",  oos.value("expectancy", 0.0)},
        {"drawdown_max",oos.value("drawdown_max", 0.0)}
      }},
      {"best_thr", m.value("best_thr", 0.0)},
      {"ma_len",   m.value("ma_len", 0)},
      {"model_path", full.value("model_path", std::string())},
      {"log_path",  m.value("log_path", std::string())},
      {"ts", m.value("build_ts", (long long)time(nullptr)*1000)}
    };
    res.set_content(out.dump(2), "application/json");
  };

  srv.Get ("/api/train/summary",  train_summary_handler);
  srv.Post("/api/train/summary",  train_summary_handler);
}
