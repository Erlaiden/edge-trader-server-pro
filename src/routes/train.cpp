#include "routes/train.h"
#include "train_logic.h"
#include "json.hpp"
#include <httplib.h>
#include <string>
#include <cstdlib>
#include <cstdio>

using json = nlohmann::json;
using namespace httplib;

static inline std::string qs(const Request& req, const char* k, const char* defv) {
    return req.has_param(k) ? req.get_param_value(k) : std::string(defv);
}
static inline int qsi(const Request& req, const char* k, int defv) {
    if (!req.has_param(k)) return defv;
    try { return std::stoi(req.get_param_value(k)); } catch (...) { return defv; }
}
static inline double qsd(const Request& req, const char* k, double defv) {
    if (!req.has_param(k)) return defv;
    try { return std::stod(req.get_param_value(k)); } catch (...) { return defv; }
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

    // формулы
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

static inline int sh(const std::string& cmd) {
    // простой запуск shell-команды с кодом возврата
    int rc = std::system(cmd.c_str());
    if (rc == -1) return 127;
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    return rc;
}

void register_train_routes(Server& svr) {
    svr.Get("/api/train", [](const Request& req, Response& res) {
        try {
            const std::string symbol   = qs(req, "symbol", "BTCUSDT");
            const std::string interval = qs(req, "interval", "15");
            int    episodes = qsi(req, "episodes", 40);
            double tp       = qsd(req, "tp",       0.008);
            double sl       = qsd(req, "sl",       0.0032);
            int    ma       = qsi(req, "ma",       12);

            // Новые управляющие параметры:
            const bool fetch    = qsi(req, "fetch",    1) != 0;  // по умолчанию качаем данные
            const int  months   = qsi(req, "months",  12);       // глубина истории для 15m
            const bool cleanup  = qsi(req, "cleanup",  0) != 0;  // удалять ли свечи после тренировки

            bool use_antimanip = qsi(req, "antimanip", 1) != 0;

            // 1) По запросу — качаем 15m и делаем агрегаты 60/240/1440
            if (fetch) {
                char cmd[1024];
                std::snprintf(
                    cmd, sizeof(cmd),
                    "/opt/edge-trader-server/scripts/fetch_15m_and_agg.sh '%s' '%d' 1>/tmp/etai_fetch.log 2>&1",
                    symbol.c_str(), months
                );
                int rc = sh(cmd);
                if (rc != 0) {
                    json err = {
                        {"ok", false},
                        {"error", "fetch_failed"},
                        {"error_detail", "fetch_15m_and_agg failed"},
                        {"symbol", symbol},
                        {"months", months}
                    };
                    res.status = 500;
                    res.set_content(err.dump(2), "application/json");
                    return;
                }
            }

            // 2) Запускаем тренировку на только что подготовленных cache/* и cache/clean/*
            json out = etai::run_train_pro_and_save(symbol, interval, episodes, tp, sl, ma, use_antimanip);
            promote_metrics(out);

            // 3) По запросу — удаляем RAW/CLEAN свечи по символу (модель остаётся)
            if (cleanup) {
                char rmcmd[1024];
                std::snprintf(
                    rmcmd, sizeof(rmcmd),
                    "sh -c 'rm -f /opt/edge-trader-server/cache/%s_*.csv /opt/edge-trader-server/cache/clean/%s_*.csv'",
                    symbol.c_str(), symbol.c_str()
                );
                (void)sh(rmcmd);
                out["cleanup_done"] = true;
            } else {
                out["cleanup_done"] = false;
            }

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
