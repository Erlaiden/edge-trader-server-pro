#include "infer.h"
#include "json.hpp"
#include "http_helpers.h"
#include "rt_metrics.h"
#include "utils.h"
#include "infer_policy.h"
#include "utils_data.h"
#include "features/features.h"
#include <armadillo>
#include <fstream>
#include <iostream>
#include <atomic>

using json = nlohmann::json;

static std::atomic<unsigned long long> REQUEST_COUNTER{0};

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

static inline void enrich_with_levels(json &out, const arma::mat& M15, double tp, double sl) {
    if (M15.n_rows >= 5 && M15.n_cols >= 1) {
        double last = (double) M15.row(4)(M15.n_cols - 1);
        out["tp_price_long"]   = last * (1.0 + tp);
        out["sl_price_long"]   = last * (1.0 - sl);
        out["tp_price_short"]  = last * (1.0 - tp);
        out["sl_price_short"]  = last * (1.0 + sl);
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
        out["ETAI_FEAT_ENABLE_MFLOW"] = (std::getenv("ETAI_FEAT_ENABLE_MFLOW")? true:false);
        res.set_content(out.dump(), "application/json");
    });

    srv.Get("/api/infer", [&](const httplib::Request& req, httplib::Response& res){
        unsigned long long req_id = REQUEST_COUNTER.fetch_add(1);
        
        // КРИТИЧНО: Логируем СРАЗУ при входе
        std::cerr << "[INFER#" << req_id << "] ========== REQUEST START ==========" << std::endl;
        
        try {
            REQ_INFER.fetch_add(1, std::memory_order_relaxed);

            std::string symbol   = qp(req, "symbol", "BTCUSDT");
            std::string interval = qp(req, "interval", "15");
            
            std::cerr << "[INFER#" << req_id << "] Params: symbol=" << symbol << " interval=" << interval << std::endl;
            
            const std::string model_path = "cache/models/" + symbol + "_" + interval + "_ppo_pro.json";

            // 1. ЗАГРУЗКА МОДЕЛИ С ДИСКА
            std::cerr << "[INFER#" << req_id << "] Opening model: " << model_path << std::endl;
            std::ifstream mf(model_path);
            if (!mf) {
                std::cerr << "[INFER#" << req_id << "] ERROR: Model file not found" << std::endl;
                json out{{"ok",false},{"error","model_not_found"},{"path",model_path}};
                res.set_content(out.dump(), "application/json");
                return;
            }
            
            json model;
            try {
                mf >> model;
                std::cerr << "[INFER#" << req_id << "] Model JSON parsed successfully" << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "[INFER#" << req_id << "] ERROR: JSON parse failed: " << e.what() << std::endl;
                json out{{"ok",false},{"error","model_parse_failed"},{"what",e.what()}};
                res.set_content(out.dump(), "application/json");
                return;
            }

            // Валидация модели
            bool model_ok = jbool(model, "ok", false);
            std::cerr << "[INFER#" << req_id << "] Model ok=" << model_ok << std::endl;
            
            if (!model_ok) {
                std::cerr << "[INFER#" << req_id << "] ERROR: Model ok=false" << std::endl;
                json out{{"ok",false},{"error","model_invalid"},{"model_ok",false}};
                res.set_content(out.dump(), "application/json");
                return;
            }

            double best_thr = jnum(model, "best_thr", 0.5);
            int    ma_len   = jint(model, "ma_len", 12);
            int    version  = jint(model, "version", 0);
            double tp       = jnum(model, "tp", 0.008);
            double sl       = jnum(model, "sl", 0.004);
            int    feat_dim = jint(model, "feat_dim", 0);

            std::cerr << "[INFER#" << req_id << "] Model params: version=" << version 
                      << " feat_dim=" << feat_dim << " thr=" << best_thr << std::endl;

            if (version == 0) {
                std::cerr << "[INFER#" << req_id << "] ERROR: version=0" << std::endl;
                json out{{"ok",false},{"error","model_no_version"}};
                res.set_content(out.dump(), "application/json");
                return;
            }

            if (feat_dim == 0) {
                std::cerr << "[INFER#" << req_id << "] ERROR: feat_dim=0" << std::endl;
                json out{{"ok",false},{"error","model_no_feat_dim"}};
                res.set_content(out.dump(), "application/json");
                return;
            }

            // 2. ЗАГРУЗКА ДАННЫХ 15M
            std::cerr << "[INFER#" << req_id << "] Loading 15m data..." << std::endl;
            arma::mat M15 = etai::load_cached_matrix(symbol, interval);
            if (M15.n_elem == 0) {
                std::cerr << "[INFER#" << req_id << "] ERROR: No cached 15m data" << std::endl;
                json out{{"ok",false},{"error","no_cached_data"}};
                res.set_content(out.dump(), "application/json");
                return;
            }
            std::cerr << "[INFER#" << req_id << "] 15m data: " << M15.n_cols << " candles" << std::endl;

            // 3. ПАРАМЕТРЫ
            double k_atr = qpd(req, "k_atr", 1.2);
            double eps   = qpd(req, "eps", 0.05);
            std::string htf = qp(req, "htf", "60,240,1440");
            
            std::set<std::string> wanted;
            { 
                std::string t; 
                for (char c: htf){ 
                    if(c==','){ 
                        if(!t.empty()){wanted.insert(t); t.clear();} 
                    } else t.push_back(c);
                } 
                if(!t.empty()) wanted.insert(t); 
            }

            // 4. ЗАГРУЗКА HTF
            arma::mat M60, M240, M1440;
            const arma::mat *p60=nullptr, *p240=nullptr, *p1440=nullptr;
            
            if (wanted.count("60")) { 
                M60 = etai::load_cached_matrix(symbol, "60");
                if (M60.n_elem) { M60 = M60.t(); p60 = &M60; }
            }
            if (wanted.count("240")) { 
                M240 = etai::load_cached_matrix(symbol, "240");
                if (M240.n_elem) { M240 = M240.t(); p240 = &M240; }
            }
            if (wanted.count("1440")) { 
                M1440 = etai::load_cached_matrix(symbol, "1440");
                if (M1440.n_elem) { M1440 = M1440.t(); p1440 = &M1440; }
            }

            // 5. ИНФЕРЕНС
            arma::mat raw15 = M15.t();
            
            std::cerr << "[INFER#" << req_id << "] Calling policy..." << std::endl;
            json inf = etai::infer_with_policy_mtf(raw15, model, p60, 12, p240, 12, p1440, 12);
            
            bool policy_ok = jbool(inf, "ok", false);
            std::string sig = jstr(inf, "signal", "UNKNOWN");
            std::cerr << "[INFER#" << req_id << "] Policy returned: ok=" << policy_ok << " signal=" << sig << std::endl;

            if (!policy_ok) {
                std::cerr << "[INFER#" << req_id << "] ERROR: Policy failed" << std::endl;
                json out{
                    {"ok", false},
                    {"error", "policy_failed"},
                    {"policy_error", jstr(inf, "error", "unknown")}
                };
                res.set_content(out.dump(), "application/json");
                return;
            }

            double score15 = jnum(inf, "score15", 0.0);

            // 6. Счётчики
            if (sig == "LONG") INFER_SIG_LONG.fetch_add(1, std::memory_order_relaxed);
            else if (sig == "SHORT") INFER_SIG_SHORT.fetch_add(1, std::memory_order_relaxed);
            else INFER_SIG_NEUTRAL.fetch_add(1, std::memory_order_relaxed);

            LAST_INFER_TS.store((long long)time(nullptr)*1000, std::memory_order_relaxed);

            // 7. РЕЖИМЫ
            double thr = best_thr > 0 ? best_thr : 0.5;
            int upVotes=0, downVotes=0;
            htf_votes(inf, upVotes, downVotes);
            int netHTF = upVotes - downVotes;

            std::string marketMode = "flat";
            double confidence = 0.0;
            double excess = std::fabs(score15) - thr;

            if (excess >= 0.0) {
                if (score15 > 0) {
                    marketMode = (netHTF >= 2) ? "trendUp" : (netHTF <= -2) ? "correction" : "trendUp";
                } else if (score15 < 0) {
                    marketMode = (netHTF <= -2) ? "trendDown" : (netHTF >= 2) ? "correction" : "trendDown";
                }
                double htfFactor = std::min(1.0, std::fabs((double)netHTF)/4.0);
                confidence = std::min(100.0, 100.0 * (0.5*std::min(1.0, excess/(thr>0?thr:0.5)) + 0.5*htfFactor));
            } else {
                double atr = atr14_from_M(M15);
                double band = k_atr * atr;

                if (score15 >= -thr*eps && score15 <= thr*eps) sig = "NEUTRAL";
                else if (score15 > thr*eps) sig = "SHORT";
                else if (score15 < -thr*eps) sig = "LONG";
                
                marketMode = "flat";
                double flatRatio = std::min(1.0, std::fabs(score15)/(thr>0?thr:0.5));
                confidence = std::min(100.0, 70.0 * flatRatio);
                inf["flat_band"] = band;
                inf["flat_k_atr"] = k_atr;
            }

            json safe_htf = (inf.contains("htf") && inf["htf"].is_object()) ? inf["htf"] : json::object();
            double safe_wctx_htf = jnum(inf, "wctx_htf", 0.0);
            double safe_vol_thr  = jnum(inf, "vol_threshold", 0.0);
            int    safe_feat_dim = jint(inf, "feat_dim_used", 0);
            bool   safe_used_norm= jbool(inf, "used_norm", true);

            json out{
                {"ok", true},
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
                {"market_mode", marketMode},
                {"confidence", confidence},
                {"htf", safe_htf},
                {"feat_dim_used", safe_feat_dim},
                {"used_norm", safe_used_norm},
                {"wctx_htf", safe_wctx_htf},
                {"vol_threshold", safe_vol_thr},
                {"agents", make_agents_summary()}
            };

            enrich_with_levels(out, M15, tp, sl);
            
            std::cerr << "[INFER#" << req_id << "] SUCCESS ==========" << std::endl;
            res.set_content(out.dump(), "application/json");
        }
        catch (const std::exception& e) {
            std::cerr << "[INFER#" << req_id << "] EXCEPTION: " << e.what() << " ==========" << std::endl;
            json out{
                {"ok", false},
                {"error", "infer_exception"},
                {"what", e.what()}
            };
            res.set_content(out.dump(), "application/json");
        }
        catch (...) {
            std::cerr << "[INFER#" << req_id << "] UNKNOWN EXCEPTION ==========" << std::endl;
            json out{
                {"ok", false},
                {"error", "infer_unknown_exception"}
            };
            res.set_content(out.dump(), "application/json");
        }
    });
}
