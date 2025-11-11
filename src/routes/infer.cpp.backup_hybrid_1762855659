#include "infer.h"
#include "json.hpp"
#include "http_helpers.h"
#include "rt_metrics.h"
#include "utils.h"
#include "infer_policy.h"
#include "utils_data.h"
#include "features/features.h"
#include "infer_cache.h"
#include <armadillo>
#include <fstream>
#include <iostream>
#include <atomic>

using json = nlohmann::json;

static std::atomic<unsigned long long> REQUEST_COUNTER{0};
static std::atomic<unsigned long long> CACHE_HITS{0};
static std::atomic<unsigned long long> CACHE_MISSES{0};
static std::atomic<unsigned long long> POLICY_FAILURES{0};

// Safe JSON helpers
static inline double jnum(const json& j, const char* k, double defv){
    if (!j.contains(k)) return defv;
    const auto& v = j.at(k);
    if (v.is_number_float() || v.is_number_integer()) return v.get<double>();
    return defv;
}
static inline int jint(const json& j, const char* k, int defv){
    if (!j.contains(k)) return defv;
    const auto& v = j.at(k);
    if (v.is_number_integer()) return v.get<int>();
    if (v.is_number_float()) return (int)v.get<double>();
    return defv;
}
static inline bool jbool(const json& j, const char* k, bool defv){
    if (!j.contains(k)) return defv;
    const auto& v = j.at(k);
    if (v.is_boolean()) return v.get<bool>();
    return defv;
}
static inline std::string jstr(const json& j, const char* k, const char* defv){
    if (!j.contains(k)) return defv;
    const auto& v = j.at(k);
    if (v.is_string()) return v.get<std::string>();
    return defv;
}

static inline double qpd(const httplib::Request& req, const char* key, double defv) {
    try {
        std::string s = qp(req, key, nullptr);
        if (s.empty()) return defv;
        size_t pos = 0;
        double v = std::stod(s, &pos);
        if (pos == 0) return defv;
        return v;
    } catch (...) { return defv; }
}

static inline json make_agents_summary() {
    return json{
        {"long_total",   (unsigned long long)INFER_SIG_LONG.load(std::memory_order_relaxed)},
        {"short_total",  (unsigned long long)INFER_SIG_SHORT.load(std::memory_order_relaxed)},
        {"neutral_total",(unsigned long long)INFER_SIG_NEUTRAL.load(std::memory_order_relaxed)}
    };
}

// ✅ ДИНАМИЧЕСКИЕ TP/SL НА ОСНОВЕ ATR
static inline void enrich_with_dynamic_levels(json &out, const arma::mat& M15, double atr) {
    if (M15.n_rows >= 5 && M15.n_cols >= 1 && atr > 0) {
        double last = (double) M15.row(4)(M15.n_cols - 1);
        
        // Адаптивные множители на основе волатильности
        double atr_percent = (atr / last) * 100.0;
        
        double tp_multiplier = 2.5;  // По умолчанию
        double sl_multiplier = 1.2;
        
        // Высокая волатильность (ATR > 3%) → шире
        if (atr_percent > 3.0) {
            tp_multiplier = 3.5;
            sl_multiplier = 1.5;
        }
        // Средняя волатильность (1.5% - 3%)
        else if (atr_percent > 1.5) {
            tp_multiplier = 3.0;
            sl_multiplier = 1.3;
        }
        // Низкая волатильность (< 1.5%)
        else {
            tp_multiplier = 2.0;
            sl_multiplier = 1.0;
        }
        
        // Рассчитываем TP/SL в процентах от цены
        double tp_percent = (atr * tp_multiplier / last);
        double sl_percent = (atr * sl_multiplier / last);
        
        out["tp"] = tp_percent;
        out["sl"] = sl_percent;
        out["atr"] = atr;
        out["atr_percent"] = atr_percent;
        
        out["tp_price_long"]   = last * (1.0 + tp_percent);
        out["sl_price_long"]   = last * (1.0 - sl_percent);
        out["tp_price_short"]  = last * (1.0 - tp_percent);
        out["sl_price_short"]  = last * (1.0 + sl_percent);
        out["last_close"]      = last;
    }
}

static inline double atr14_from_M(const arma::mat& M) {
    if (M.n_cols < 16 || M.n_rows < 5) return 0.0;
    size_t N = M.n_cols;
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

static inline void htf_votes(const json& inf, int& up, int& down) {
    up=down=0;
    if (!inf.contains("htf") || !inf["htf"].is_object()) return;
    for (auto& kv : inf["htf"].items()) {
        const json& h = kv.value();
        double sc = jnum(h, "score", 0.0);
        bool strong = jbool(h, "strong", false);
        if (sc > 0) { if (strong) up+=2; else up+=1; }
        else if (sc < 0) { if (strong) down+=2; else down+=1; }
    }
}

void register_infer_routes(httplib::Server& srv) {
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
        out["raw_rows"] = (int)raw.n_rows;
        out["raw_cols"] = (int)raw.n_cols;
        out["F_rows"]   = (int)F.n_rows;
        out["F_cols"]   = (int)F.n_cols;
        res.set_content(out.dump(), "application/json");
    });

    srv.Get("/api/infer/stats", [&](const httplib::Request&, httplib::Response& res){
        json out{
            {"cache_hits", (unsigned long long)CACHE_HITS.load()},
            {"cache_misses", (unsigned long long)CACHE_MISSES.load()},
            {"policy_failures", (unsigned long long)POLICY_FAILURES.load()},
            {"total_requests", (unsigned long long)REQUEST_COUNTER.load()}
        };
        res.set_content(out.dump(), "application/json");
    });

    srv.Post("/api/infer/cache/clear", [&](const httplib::Request&, httplib::Response& res){
        etai::get_infer_cache().clear();
        json out{{"ok", true}, {"message", "cache_cleared"}};
        res.set_content(out.dump(), "application/json");
    });

    // Альтернативный GET endpoint для очистки кэша
    srv.Get("/api/cache/clear", [&](const httplib::Request&, httplib::Response& res){
        etai::get_infer_cache().clear();
        json out{{"ok", true}, {"message", "cache_cleared"}};
        res.set_content(out.dump(), "application/json");
    });

    srv.Get("/api/infer", [&](const httplib::Request& req, httplib::Response& res){
        unsigned long long req_id = REQUEST_COUNTER.fetch_add(1);

        try {
            REQ_INFER.fetch_add(1, std::memory_order_relaxed);

            std::string symbol   = qp(req, "symbol", "BTCUSDT");
            std::string interval = qp(req, "interval", "15");

            // 1. КЭSH
            etai::CachedInferData cached;
            bool from_cache = etai::get_infer_cache().get(symbol, interval, cached);

            if (from_cache) {
                CACHE_HITS.fetch_add(1);
            } else {
                CACHE_MISSES.fetch_add(1);

                const std::string model_path = "cache/models/" + symbol + "_" + interval + "_ppo_pro.json";
                std::ifstream mf(model_path);
                if (!mf) {
                    json out{{"ok",false},{"error","model_not_found"}};
                    res.set_content(out.dump(), "application/json");
                    return;
                }

                try {
                    mf >> cached.model;
                } catch (const std::exception& e) {
                    json out{{"ok",false},{"error","model_parse_failed"}};
                    res.set_content(out.dump(), "application/json");
                    return;
                }

                if (!jbool(cached.model, "ok", false)) {
                    json out{{"ok",false},{"error","model_invalid"}};
                    res.set_content(out.dump(), "application/json");
                    return;
                }

                cached.M15 = etai::load_cached_matrix(symbol, interval);
                if (cached.M15.n_elem == 0) {
                    json out{{"ok",false},{"error","no_cached_data"}};
                    res.set_content(out.dump(), "application/json");
                    return;
                }

                cached.M60 = etai::load_cached_matrix(symbol, "60");
                cached.M240 = etai::load_cached_matrix(symbol, "240");
                cached.M1440 = etai::load_cached_matrix(symbol, "1440");

                cached.loaded_at = std::chrono::steady_clock::now();
                etai::get_infer_cache().put(symbol, interval, cached);
            }

            int version = jint(cached.model, "version", 0);
            int feat_dim = jint(cached.model, "feat_dim", 0);
            double best_thr = jnum(cached.model, "best_thr", 0.5);
            int ma_len = jint(cached.model, "ma_len", 12);

            if (version == 0 || feat_dim == 0) {
                json out{{"ok",false},{"error","model_incomplete"}};
                res.set_content(out.dump(), "application/json");
                return;
            }

            // 2. РАСЧЁТ ATR
            double atr = atr14_from_M(cached.M15);

            // 3. ИНФЕРЕНС С RETRY
            arma::mat M60_t, M240_t, M1440_t;
            const arma::mat *p60 = nullptr, *p240 = nullptr, *p1440 = nullptr;

            if (cached.M60.n_elem) { M60_t = cached.M60.t(); p60 = &M60_t; }
            if (cached.M240.n_elem) { M240_t = cached.M240.t(); p240 = &M240_t; }
            if (cached.M1440.n_elem) { M1440_t = cached.M1440.t(); p1440 = &M1440_t; }

            arma::mat raw15 = cached.M15.t();

            json inf;
            bool policy_success = false;

            // RETRY до 3 раз
            for (int attempt = 0; attempt < 3 && !policy_success; ++attempt) {
                try {
                    inf = etai::infer_with_policy_mtf(raw15, cached.model, p60, 12, p240, 12, p1440, 12);

                    // Валидация результата
                    if (jbool(inf, "ok", false) &&
                        jint(inf, "feat_dim_used", 0) > 0 &&
                        inf.contains("htf")) {
                        policy_success = true;
                    } else {
                        std::cerr << "[INFER#" << req_id << "] Policy returned invalid result, attempt " << (attempt+1) << std::endl;
                        if (attempt < 2) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                } catch (const std::exception& e) {
                    std::cerr << "[INFER#" << req_id << "] Policy exception: " << e.what() << ", attempt " << (attempt+1) << std::endl;
                    if (attempt < 2) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                } catch (...) {
                    std::cerr << "[INFER#" << req_id << "] Policy unknown exception, attempt " << (attempt+1) << std::endl;
                    if (attempt < 2) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }

            if (!policy_success) {
                POLICY_FAILURES.fetch_add(1);
                json out{{"ok", false},{"error", "policy_failed_after_retries"}};
                res.set_content(out.dump(), "application/json");
                return;
            }

            std::string sig = jstr(inf, "signal", "NEUTRAL");
            double score15 = jnum(inf, "score15", 0.0);

            if (sig == "LONG") INFER_SIG_LONG.fetch_add(1, std::memory_order_relaxed);
            else if (sig == "SHORT") INFER_SIG_SHORT.fetch_add(1, std::memory_order_relaxed);
            else INFER_SIG_NEUTRAL.fetch_add(1, std::memory_order_relaxed);

            LAST_INFER_TS.store((long long)time(nullptr)*1000, std::memory_order_relaxed);

            // Режимы
            double k_atr = qpd(req, "k_atr", 1.2);
            double eps = qpd(req, "eps", 0.05);
            double thr = best_thr > 0 ? best_thr : 0.5;

            int upVotes=0, downVotes=0;
            htf_votes(inf, upVotes, downVotes);
            int netHTF = upVotes - downVotes;

            std::string marketMode = "flat";
            double confidence = 0.0;
            double excess = std::fabs(score15) - thr;

            if (excess >= 0.0) {
                if (score15 > 0) marketMode = (netHTF >= 2) ? "trendUp" : (netHTF <= -2) ? "correction" : "trendUp";
                else if (score15 < 0) marketMode = (netHTF <= -2) ? "trendDown" : (netHTF >= 2) ? "correction" : "trendDown";

                double htfFactor = std::min(1.0, std::fabs((double)netHTF)/4.0);
                confidence = std::min(100.0, 100.0 * (0.5*std::min(1.0, excess/(thr>0?thr:0.5)) + 0.5*htfFactor));
            } else {
                double band = k_atr * atr;

                if (score15 >= -thr*eps && score15 <= thr*eps) sig = "NEUTRAL";
                else if (score15 > thr*eps) sig = "SHORT";
                else if (score15 < -thr*eps) sig = "LONG";

                marketMode = "flat";
                confidence = std::min(100.0, 70.0 * std::min(1.0, std::fabs(score15)/(thr>0?thr:0.5)));
                inf["flat_band"] = band;
                inf["flat_k_atr"] = k_atr;
            }

            json safe_htf = (inf.contains("htf") && inf["htf"].is_object()) ? inf["htf"] : json::object();

            json out{
                {"ok", true},
                {"mode", "pro"},
                {"symbol", symbol},
                {"interval", interval},
                {"version", version},
                {"thr", best_thr},
                {"ma_len", ma_len},
                {"signal", sig},
                {"score15", score15},
                {"market_mode", marketMode},
                {"confidence", confidence},
                {"htf", safe_htf},
                {"feat_dim_used", jint(inf, "feat_dim_used", 0)},
                {"used_norm", jbool(inf, "used_norm", true)},
                {"wctx_htf", jnum(inf, "wctx_htf", 0.0)},
                {"vol_threshold", jnum(inf, "vol_threshold", 0.0)},
                {"agents", make_agents_summary()},
                {"from_cache", from_cache}
            };

            // ✅ ДИНАМИЧЕСКИЕ TP/SL НА ОСНОВЕ ATR
            enrich_with_dynamic_levels(out, cached.M15, atr);
            
            res.set_content(out.dump(), "application/json");
        }
        catch (const std::exception& e) {
            std::cerr << "[INFER#" << req_id << "] EXCEPTION: " << e.what() << std::endl;
            json out{{"ok", false},{"error", "infer_exception"},{"what", e.what()}};
            res.set_content(out.dump(), "application/json");
        }
        catch (...) {
            std::cerr << "[INFER#" << req_id << "] UNKNOWN EXCEPTION" << std::endl;
            json out{{"ok", false},{"error", "infer_unknown_exception"}};
            res.set_content(out.dump(), "application/json");
        }
    });
}
