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
#include <cmath>

using json = nlohmann::json;

// counters → summary
static inline json make_agents_summary() {
    return json{
        {"long_total",   (unsigned long long)INFER_SIG_LONG.load(std::memory_order_relaxed)},
        {"short_total",  (unsigned long long)INFER_SIG_SHORT.load(std::memory_order_relaxed)},
        {"neutral_total",(unsigned long long)INFER_SIG_NEUTRAL.load(std::memory_order_relaxed)}
    };
}

// last close + tp/sl levels
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

// ATR(14) по 6×N матрице (rows: ts,open,high,low,close,vol)
static inline double atr14_from_M(const arma::mat& M) {
    if (M.n_cols < 16 || M.n_rows < 5) return 0.0;
    size_t N = M.n_cols;
    double atr = 0.0;
    double prevClose = (double)M.row(4)(N-16);
    double alpha = 1.0/14.0;
    double ema = 0.0;
    bool init=false;

    for (size_t i = N-15; i < N; ++i) {
        double hi = (double)M.row(2)(i);
        double lo = (double)M.row(3)(i);
        double cl = (double)M.row(4)(i);
        double tr = std::max({hi - lo, std::fabs(hi - prevClose), std::fabs(lo - prevClose)});
        if (!init) { ema = tr; init = true; }
        else { ema = alpha*tr + (1.0-alpha)*ema; }
        prevClose = cl;
    }
    return ema;
}

// Простой разбор HTF-голосов
static inline void htf_votes(const json& inf, int& up, int& down) {
    up=down=0;
    if (!inf.contains("htf") || !inf["htf"].is_object()) return;
    for (auto& kv : inf["htf"].items()) {
        const json& h = kv.value();
        // по score: >0 ⇒ up, <0 ⇒ down
        double sc = h.value("score", 0.0);
        bool strong = h.value("strong", false);
        if (sc > 0) { if (strong) up+=2; else up+=1; }
        else if (sc < 0) { if (strong) down+=2; else down+=1; }
    }
}

void register_infer_routes(httplib::Server& srv) {
    // DIAG: фичи
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

    // --- MAIN: /api/infer ---
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

        // Кэш 6×N (ts,open,high,low,close,vol)
        arma::mat M15 = etai::load_cached_matrix(symbol, interval);
        if (M15.n_elem == 0) {
            json out{{"ok",false},{"error","no_cached_data"},{"hint","call /api/backfill first"}};
            res.set_content(out.dump(), "application/json");
            return;
        }

        // Параметры flat-коридора
        double k_atr = qp(req, "k_atr", 1.2);
        double eps   = qp(req, "eps",   0.05);

        // HTF список
        std::string htf = qp(req, "htf", "60,240,1440");
        std::set<std::string> wanted;
        { std::string t; for (char c: htf){ if(c==','){ if(!t.empty()){wanted.insert(t); t.clear();} } else t.push_back(c);} if(!t.empty()) wanted.insert(t); }

        // HTF матрицы как N×6
        arma::mat M60, M240, M1440;
        const arma::mat *p60=nullptr, *p240=nullptr, *p1440=nullptr;
        if (wanted.count("60"))   { M60    = etai::load_cached_matrix(symbol, "60");   if (M60.n_elem)    { M60 = M60.t();    p60    = &M60; } }
        if (wanted.count("240"))  { M240   = etai::load_cached_matrix(symbol, "240");  if (M240.n_elem)   { M240 = M240.t();  p240   = &M240; } }
        if (wanted.count("1440")) { M1440  = etai::load_cached_matrix(symbol, "1440"); if (M1440.n_elem)  { M1440 = M1440.t();p1440  = &M1440; } }

        // 15m → N×6
        arma::mat raw15 = M15.t();

        // policy-инфер
        json inf = etai::infer_with_policy_mtf(raw15, model, p60, 12, p240, 12, p1440, 12);

        std::string sig = inf.value("signal","NEUTRAL");
        double score15  = inf.value("score15", 0.0);

        if (sig == "LONG") INFER_SIG_LONG.fetch_add(1, std::memory_order_relaxed);
        else if (sig == "SHORT") INFER_SIG_SHORT.fetch_add(1, std::memory_order_relaxed);
        else INFER_SIG_NEUTRAL.fetch_add(1, std::memory_order_relaxed);

        LAST_INFER_TS.store((long long)time(nullptr)*1000, std::memory_order_relaxed);

        // last close
        double last_close = 0.0;
        if (M15.n_rows >= 5 && M15.n_cols >= 1) {
            last_close = (double) M15.row(4)(M15.n_cols - 1);
        }

        // --- РЕЖИМЫ + УВЕРЕННОСТЬ ---
        // Базовый порог
        double thr = best_thr > 0 ? best_thr : 0.5;

        // Голоса HTF
        int upVotes=0, downVotes=0;
        htf_votes(inf, upVotes, downVotes);
        int netHTF = upVotes - downVotes;

        std::string marketMode = "flat"; // по умолчанию
        double confidence = 0.0;         // 0..100

        // 1) тренд/коррекция на основе score и HTF
        double excess = std::fabs(score15) - thr; // насколько выше порога
        if (excess >= 0.0) {
            if (score15 > 0) {
                if (netHTF >= 2) { marketMode = "trendUp"; }
                else if (netHTF <= -2) { marketMode = "correction"; }
                else { marketMode = "trendUp"; }
            } else if (score15 < 0) {
                if (netHTF <= -2) { marketMode = "trendDown"; }
                else if (netHTF >= 2) { marketMode = "correction"; }
                else { marketMode = "trendDown"; }
            }
            // уверенность тренда: из excess и согласия HTF
            double htfFactor = std::min(1.0, std::fabs((double)netHTF)/4.0); // 0..1
            confidence = std::min(100.0, 100.0 * (0.5*std::min(1.0, excess/(thr>0?thr:0.5)) + 0.5*htfFactor));
        } else {
            // 2) flat: коридор по ATR
            double atr = atr14_from_M(M15);
            double band = k_atr * atr;
            double upper = last_close + band;
            double lower = last_close - band;

            // расстояние цены до границы/середины неизвестно → используем score как прокси:
            // если score15>0 => тянет вверх, <0 => вниз. Ставим сигналы у границ.
            if (score15 >= -thr*eps && score15 <= thr*eps) {
                sig = "NEUTRAL";
            } else if (score15 > thr*eps) {
                // верхняя зона → SHORT
                sig = "SHORT";
            } else if (score15 < -thr*eps) {
                // нижняя зона → LONG
                sig = "LONG";
            }
            marketMode = "flat";
            // уверенность flat: чем больше |score15| к thr, тем выше
            double flatRatio = std::min(1.0, std::fabs(score15)/(thr>0?thr:0.5));
            confidence = std::min(100.0, 70.0 * flatRatio);
            // прокинем коридор для клиента при желании
            inf["flat_band"] = band;
            inf["flat_k_atr"] = k_atr;
        }

        // --- Ответ ---
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
            {"score15", score15},
            {"htf", inf["htf"]},
            {"feat_dim_used", inf.value("feat_dim_used", 0)},
            {"used_norm", inf.value("used_norm", true)},
            {"wctx_htf", inf.value("wctx_htf", 0.0)},
            {"vol_threshold", inf.value("vol_threshold", 0.0)},
            {"agents", make_agents_summary()},
            {"marketMode", marketMode},
            {"confidence", confidence},
        };

        enrich_with_levels(out, M15, tp, sl); // добавит last_close и tp/sl цены

        // для обратной совместимости оставим price и threshold
        out["price"] = out.value("last_close", 0.0);
        out["threshold"] = best_thr;

        res.set_content(out.dump(), "application/json");
    });
}
