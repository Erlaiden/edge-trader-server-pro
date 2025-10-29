#include "infer.h"
#include "json.hpp"
#include "http_helpers.h"
#include "rt_metrics.h"
#include "ppo.h"
#include "utils.h"
#include "infer_policy.h"     // << добавлено
#include <armadillo>
#include <set>
#include <fstream>
#include <ctime>

using json = nlohmann::json;

static inline json make_agents_summary() {
    return json{
        {"long_total",   (unsigned long long)INFER_SIG_LONG.load(std::memory_order_relaxed)},
        {"short_total",  (unsigned long long)INFER_SIG_SHORT.load(std::memory_order_relaxed)},
        {"neutral_total",(unsigned long long)INFER_SIG_NEUTRAL.load(std::memory_order_relaxed)}
    };
}

static inline void enrich_with_levels(json &out, const arma::mat& M15, double tp, double sl) {
    if (M15.n_rows >= 5 && M15.n_cols >= 1) {
        double last = (double) M15.row(4)(M15.n_cols - 1); // close
        out["tp_price_long"]   = last * (1.0 + tp);
        out["sl_price_long"]   = last * (1.0 - sl);
        out["tp_price_short"]  = last * (1.0 - tp);
        out["sl_price_short"]  = last * (1.0 + sl);
        out["last_close"]      = last;
    }
}

void register_infer_routes(httplib::Server& srv) {
    // /api/infer — single TF + опционально MTF через ?htf=60,240,1440
    srv.Get("/api/infer", [&](const httplib::Request& req, httplib::Response& res){
        REQ_INFER.fetch_add(1, std::memory_order_relaxed);

        std::string symbol   = qp(req, "symbol", "BTCUSDT");
        std::string interval = qp(req, "interval", "15");
        const std::string path = "cache/models/" + symbol + "_" + interval + "_ppo_pro.json";

        std::ifstream f(path);
        if (!f) {
            json out{{"ok",false},{"error","model_not_found"},{"path",path}};
            res.set_content(out.dump(), "application/json");
            return;
        }
        json model; f >> model;

        double best_thr = model.value("best_thr", 0.0);
        int    ma_len   = model.value("ma_len", 12);
        int    version  = model.value("version", 3);
        double tp       = model.value("tp", 0.0);
        double sl       = model.value("sl", 0.0);

        arma::mat M15 = etai::load_cached_matrix(symbol, interval);
        if (M15.n_elem == 0) {
            json out{{"ok",false},{"error","no_cached_data"},{"hint","call /api/backfill first"}};
            res.set_content(out.dump(), "application/json");
            return;
        }

        std::string htf = qp(req, "htf", "");
        if (!htf.empty()) {
            std::set<std::string> wanted;
            { std::string t; for (char c: htf){ if(c==','){ if(!t.empty()){wanted.insert(t); t.clear();} } else t.push_back(c);} if(!t.empty()) wanted.insert(t); }

            arma::mat M60, M240, M1440;
            const arma::mat *p60=nullptr, *p240=nullptr, *p1440=nullptr;
            int ma60   = qp(req,"ma60").empty()   ? 12 : std::atoi(qp(req,"ma60").c_str());
            int ma240  = qp(req,"ma240").empty()  ? 12 : std::atoi(qp(req,"ma240").c_str());
            int ma1440 = qp(req,"ma1440").empty() ? 12 : std::atoi(qp(req,"ma1440").c_str());

            if (wanted.count("60"))   { M60    = etai::load_cached_matrix(symbol, "60");   if (M60.n_elem)    p60    = &M60; }
            if (wanted.count("240"))  { M240   = etai::load_cached_matrix(symbol, "240");  if (M240.n_elem)   p240   = &M240; }
            if (wanted.count("1440")) { M1440  = etai::load_cached_matrix(symbol, "1440"); if (M1440.n_elem)  p1440  = &M1440; }

            json inf = etai::infer_mtf(M15, best_thr, ma_len, p60, ma60, p240, ma240, p1440, ma1440);
            std::string sig = inf.value("signal","NEUTRAL");
            if (sig == "LONG") INFER_SIG_LONG.fetch_add(1, std::memory_order_relaxed);
            else if (sig == "SHORT") INFER_SIG_SHORT.fetch_add(1, std::memory_order_relaxed);
            else INFER_SIG_NEUTRAL.fetch_add(1, std::memory_order_relaxed);

            LAST_INFER_TS.store((long long)time(nullptr)*1000, std::memory_order_relaxed);

            json out{
                {"ok", inf.value("ok", false)},
                {"mode", "pro"},
                {"symbol", symbol},
                {"interval", interval},
                {"version", version},
                {"thr", best_thr},
                {"ma_len", ma_len},
                {"tp", tp},
                {"sl", sl},
                {"signal", sig},
                {"score15", inf.value("score15", 0.0)},
                {"sigma15", inf.value("sigma15", 0.0)},
                {"vol_threshold", inf.value("vol_threshold", 0.0)},
                {"htf", inf.value("htf", json::object())},
                {"agents", make_agents_summary()}
            };
            enrich_with_levels(out, M15, tp, sl);
            res.set_content(out.dump(), "application/json");
            return;
        }

        // ===== single-TF =====
        json inf;
        bool used_policy = false;
        if (model.contains("policy")) {
            // Пытаемся инферить по policy (новый путь)
            inf = etai::infer_with_policy(M15, model);
            used_policy = inf.value("ok", false);
        }
        if (!used_policy) {
            // Fallback: старый путь по порогу best_thr
            inf = etai::infer_with_threshold(M15, best_thr, ma_len);
        }

        std::string sig = inf.value("signal","NEUTRAL");
        if (sig == "LONG") INFER_SIG_LONG.fetch_add(1, std::memory_order_relaxed);
        else if (sig == "SHORT") INFER_SIG_SHORT.fetch_add(1, std::memory_order_relaxed);
        else INFER_SIG_NEUTRAL.fetch_add(1, std::memory_order_relaxed);

        LAST_INFER_TS.store((long long)time(nullptr)*1000, std::memory_order_relaxed);

        json out{
            {"ok", inf.value("ok", false)},
            {"mode", "pro"},
            {"symbol", symbol},
            {"interval", interval},
            {"version", version},
            {"thr", best_thr},
            {"ma_len", ma_len},
            {"tp", tp},
            {"sl", sl},
            {"signal", sig},
            {"score", inf.value("score", 0.0)},
            {"sigma", inf.value("sigma", 0.0)},
            {"vol_threshold", inf.value("vol_threshold", 0.0)},
            {"agents", make_agents_summary()},
            {"used_policy", used_policy}
        };
        enrich_with_levels(out, M15, tp, sl);
        res.set_content(out.dump(), "application/json");
    });

    // /api/infer/batch — оставляем без policy (пока), добавлены tp/sl и agents
    srv.Get("/api/infer/batch", [&](const httplib::Request& req, httplib::Response& res){
        REQ_INFER.fetch_add(1, std::memory_order_relaxed);

        std::string symbol   = qp(req, "symbol", "BTCUSDT");
        std::string interval = qp(req, "interval", "15");
        int n = 120; if (!qp(req,"n").empty()) n = std::atoi(qp(req,"n").c_str());

        const std::string path = "cache/models/" + symbol + "_" + interval + "_ppo_pro.json";
        std::ifstream f(path);
        if (!f) {
            json out{{"ok",false},{"error","model_not_found"},{"path",path}};
            res.set_content(out.dump(), "application/json");
            return;
        }
        json model; f >> model;

        double thr15  = model.value("best_thr", 0.0);
        int    ma15   = model.value("ma_len", 12);
        int    version= model.value("version", 3);
        double tp     = model.value("tp", 0.0);
        double sl     = model.value("sl", 0.0);

        arma::mat M15 = etai::load_cached_matrix(symbol, interval);
        if (M15.n_elem == 0) {
            json out{{"ok",false},{"error","no_cached_data"},{"hint","call /api/backfill first"}};
            res.set_content(out.dump(), "application/json");
            return;
        }

        std::string htf = qp(req, "htf", "");
        std::set<std::string> wanted;
        if (!htf.empty()) {
            std::string t; for (char c: htf){ if(c==','){ if(!t.empty()){wanted.insert(t); t.clear();} } else t.push_back(c);} if(!t.empty()) wanted.insert(t);
        }

        arma::mat M60, M240, M1440;
        const arma::mat *p60=nullptr, *p240=nullptr, *p1440=nullptr;
        int ma60   = qp(req,"ma60").empty()   ? 12 : std::atoi(qp(req,"ma60").c_str());
        int ma240  = qp(req,"ma240").empty()  ? 12 : std::atoi(qp(req,"ma240").c_str());
        int ma1440 = qp(req,"ma1440").empty() ? 12 : std::atoi(qp(req,"ma1440").c_str());

        if (wanted.count("60"))   { M60    = etai::load_cached_matrix(symbol, "60");   if (M60.n_elem)    p60    = &M60; }
        if (wanted.count("240"))  { M240   = etai::load_cached_matrix(symbol, "240");  if (M240.n_elem)   p240   = &M240; }
        if (wanted.count("1440")) { M1440  = etai::load_cached_matrix(symbol, "1440"); if (M1440.n_elem)  p1440  = &M1440; }

        json batch = etai::infer_mtf_batch(M15, thr15, ma15, p60, ma60, p240, ma240, p1440, ma1440, n);

        long long ts_start = (long long)M15.row(0)(std::max<size_t>(0, M15.n_cols - (size_t)n));
        long long ts_end   = (long long)M15.row(0)(M15.n_cols-1);

        batch["ok"] = batch.value("ok", false);
        batch["mode"] = "pro";
        batch["symbol"] = symbol;
        batch["version"] = version;
        batch["ts_start"] = ts_start;
        batch["ts_end"] = ts_end;
        batch["bars_requested"] = n;
        batch["bars_returned"]  = batch.value("n", 0);
        batch["thr"] = thr15;
        batch["ma_len"] = ma15;
        batch["tp"] = tp;
        batch["sl"] = sl;
        batch["agents"] = make_agents_summary();
        enrich_with_levels(batch, M15, tp, sl);

        LAST_INFER_TS.store((long long)time(nullptr)*1000, std::memory_order_relaxed);
        res.set_content(batch.dump(), "application/json");
    });
}
