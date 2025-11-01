#include "infer.h"
#include "json.hpp"
#include "http_helpers.h"
#include "rt_metrics.h"
#include "ppo.h"
#include "utils.h"
#include "infer_policy.h"
#include "utils_data.h"
#include "features/features.h"
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
    // DIAG: реальный D фич
    srv.Get("/api/infer/feat_cols", [&](const httplib::Request& req, httplib::Response& res){
        std::string symbol   = qp(req, "symbol", "BTCUSDT");
        std::string interval = qp(req, "interval", "15");
        arma::mat raw;
        json out{{"ok", false}};
        if (!etai::load_raw_ohlcv(symbol, interval, raw)) {
            out["error"] = "load_raw_failed";
            res.set_content(out.dump(), "application/json");
            return;
        }
        arma::mat F = etai::build_feature_matrix(raw);
        out["ok"] = true;
        out["raw_rows"] = (int)raw.n_rows;   out["raw_cols"] = (int)raw.n_cols;
        out["F_rows"]   = (int)F.n_rows;     out["F_cols"]   = (int)F.n_cols;
        out["ETAI_FEAT_ENABLE_MFLOW"] = (std::getenv("ETAI_FEAT_ENABLE_MFLOW")? true:false);
        res.set_content(out.dump(), "application/json");
    });

    // --- /api/infer
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

        // Кэшированная матрица 6×N (ряды = ts,open,high,low,close,vol)
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

            // HTF грузим как 6×N и ТРАНСПОНИРУЕМ в N×6 для policy
            arma::mat M60, M240, M1440;
            const arma::mat *p60=nullptr, *p240=nullptr, *p1440=nullptr;

            if (wanted.count("60"))   { M60    = etai::load_cached_matrix(symbol, "60");   if (M60.n_elem)    { M60 = M60.t();    p60    = &M60; } }
            if (wanted.count("240"))  { M240   = etai::load_cached_matrix(symbol, "240");  if (M240.n_elem)   { M240 = M240.t();  p240   = &M240; } }
            if (wanted.count("1440")) { M1440  = etai::load_cached_matrix(symbol, "1440"); if (M1440.n_elem)  { M1440 = M1440.t();p1440  = &M1440; } }

            // 15m → N×6
            arma::mat raw15 = M15.t();

            json inf = etai::infer_with_policy_mtf(raw15, model, p60, 12, p240, 12, p1440, 12);

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
                {"wctx_htf", inf.value("wctx_htf", 1.0)},
                {"vol_threshold", inf.value("vol_threshold", 0.0)},
                {"htf", inf.value("htf", json::object())},
                {"used_norm", inf.value("used_norm", false)},
                {"feat_dim_used", inf.value("feat_dim_used", 0)},
                {"agents", make_agents_summary()}
            };
            enrich_with_levels(out, M15, tp, sl);
            res.set_content(out.dump(), "application/json");
            return;
        }

        // ===== single-TF policy (N×6)
        arma::mat raw_for_policy = M15.t();
        json inf_pol = etai::infer_with_policy(raw_for_policy, model);
        bool used_policy = inf_pol.value("ok", false);

        // Фоллбек по порогу
        json inf = used_policy ? inf_pol : etai::infer_with_threshold(M15, best_thr, ma_len);

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
            {"used_policy", used_policy},
            {"used_norm", inf.value("used_norm", nullptr)},
            {"feat_dim_used", inf.value("feat_dim_used", nullptr)}
        };

        if (!used_policy) {
            out["policy_dbg"] = inf_pol;
            arma::mat rawN6;
            if (etai::load_raw_ohlcv(symbol, interval, rawN6)) {
                arma::mat F = etai::build_feature_matrix(rawN6);
                out["feat_probe"] = json{
                    {"raw_rows", (int)rawN6.n_rows},
                    {"raw_cols", (int)rawN6.n_cols},
                    {"F_rows",   (int)F.n_rows},
                    {"F_cols",   (int)F.n_cols},
                    {"ETAI_FEAT_ENABLE_MFLOW", (bool)(std::getenv("ETAI_FEAT_ENABLE_MFLOW")?true:false)}
                };
            }
        }

        enrich_with_levels(out, M15, tp, sl);
        res.set_content(out.dump(), "application/json");
    });

    // --- batch (диагностика; оставляем пороговый пайп, но ДОБАВЛЯЕМ ma60/ma240/ma1440)
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
