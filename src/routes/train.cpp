#include "routes/train.h"
#include "train_logic.h"
#include "json.hpp"
#include <httplib.h>
#include <string>

using json = nlohmann::json;
using namespace httplib;

// Достаём параметр из querystring
static inline std::string qs(const Request& req, const char* k, const char* defv) {
    return req.has_param(k) ? req.get_param_value(k) : std::string(defv);
}

// Дублируем ключевые метрики на верхний уровень ответа
static void promote_metrics(json& j) {
    if (!j.contains("metrics") || !j["metrics"].is_object()) return;
    const json& m = j["metrics"];
    auto copy = [&](const char* k) {
        if (m.contains(k)) j[k] = m.at(k);
    };

    // базовые
    copy("val_accuracy");
    copy("val_reward");
    copy("val_reward_v2");
    copy("M_labeled");
    copy("val_size");
    copy("N_rows");
    copy("raw_cols");
    copy("feat_cols");
    copy("best_thr");

    // расширенные Reward v2
    copy("val_profit_avg");
    copy("val_sharpe");
    copy("val_winrate");
    copy("val_drawdown");

    // конфиги формулы (в том числе эффективные)
    copy("fee_per_trade");
    copy("alpha_sharpe");
    copy("lambda_risk");
    copy("mu_manip");
    copy("val_lambda_eff");
    copy("val_mu_eff");

    // анти-манип
    copy("manip_seen");
    copy("manip_rejected");
    copy("val_manip_ratio");

    // контекст/MTF
    copy("val_reward_wctx");
    copy("wctx_htf");
    copy("htf_agree60");
    copy("htf_agree240");
}

void register_train_routes(Server& svr) {
    svr.Get("/api/train", [](const Request& req, Response& res) {
        try {
            const std::string symbol   = qs(req, "symbol", "BTCUSDT");
            const std::string interval = qs(req, "interval", "15");
            int episodes = 40;
            double tp = 0.008, sl = 0.0032;
            int ma = 12;

            try { episodes = std::stoi(qs(req,"episodes","40")); } catch(...) {}
            try { tp       = std::stod(qs(req,"tp","0.008"));   } catch(...) {}
            try { sl       = std::stod(qs(req,"sl","0.0032"));  } catch(...) {}
            try { ma       = std::stoi(qs(req,"ma","12"));      } catch(...) {}

            bool use_antimanip = true;
            try { use_antimanip = std::stoi(qs(req,"antimanip","1")) != 0; } catch(...) {}

            json out = etai::run_train_pro_and_save(symbol, interval, episodes, tp, sl, ma, use_antimanip);
            promote_metrics(out);
            res.set_content(out.dump(2), "application/json");
        } catch (const std::exception& e) {
            json err = {{"ok", false}, {"error", "train_handler_exception"}, {"error_detail", e.what()}};
            res.set_content(err.dump(2), "application/json");
        } catch (...) {
            json err = {{"ok", false}, {"error", "train_handler_unknown"}, {"error_detail", "unknown"}};
            res.set_content(err.dump(2), "application/json");
        }
    });
}
