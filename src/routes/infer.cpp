#include "infer.h"
#include "json.hpp"
#include "http_helpers.h"
#include "rt_metrics.h"
#include "utils.h"
#include "infer_policy.h"
#include "../ppo_rl.h"
#include "utils_data.h"
#include "features/features.h"
#include "infer_cache.h"
#include "../market/regime_detector.h"
#include <armadillo>
#include "../market/volume_analysis.h"
#include "../market/candlestick_patterns.h"
#include "../market/open_interest.h"
#include "../market/support_resistance.h"
#include <fstream>
#include "../market/funding_rate.h"
#include "../market/order_book.h"
#include <iostream>
#include "../market/volatility_regime.h"
#include <atomic>
#include "../market/dynamic_calibration.h"

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

static inline double avg_atr_from_M(const arma::mat& M) {
    if (M.n_cols < 50 || M.n_rows < 5) return 0.0;
    size_t N = M.n_cols;
    double sum = 0.0;
    for (size_t i = N-50; i < N; ++i) {
        double hi = (double)M.row(2)(i);
        double lo = (double)M.row(3)(i);
        sum += (hi - lo);
    }
    return sum / 50.0;
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

        out["ok"]      = true;
        out["raw_rows"]= (int)raw.n_rows;
        out["raw_cols"]= (int)raw.n_cols;
        out["F_rows"]  = (int)F.n_rows;
        out["F_cols"]  = (int)F.n_cols;
        res.set_content(out.dump(), "application/json");
    });

    srv.Get("/api/infer/stats", [&](const httplib::Request&, httplib::Response& res){
        json out{
            {"cache_hits",      (unsigned long long)CACHE_HITS.load()},
            {"cache_misses",    (unsigned long long)CACHE_MISSES.load()},
            {"policy_failures", (unsigned long long)POLICY_FAILURES.load()},
            {"total_requests",  (unsigned long long)REQUEST_COUNTER.load()}
        };
        res.set_content(out.dump(), "application/json");
    });

    srv.Post("/api/infer/cache/clear", [&](const httplib::Request&, httplib::Response& res){
        etai::get_infer_cache().clear();
        json out{{"ok", true}, {"message", "cache_cleared"}};
        res.set_content(out.dump(), "application/json");
    });

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
            int leverage = (int)qpd(req, "leverage", 10.0);

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

                cached.M15   = etai::load_cached_matrix(symbol, interval);
                if (cached.M15.n_elem == 0) {
                    json out{{"ok",false},{"error","no_cached_data"}};
                    res.set_content(out.dump(), "application/json");
                    return;
                }

                cached.M60   = etai::load_cached_matrix(symbol, "60");
                cached.M240  = etai::load_cached_matrix(symbol, "240");
                cached.M1440 = etai::load_cached_matrix(symbol, "1440");

                cached.loaded_at = std::chrono::steady_clock::now();
                etai::get_infer_cache().put(symbol, interval, cached);
            }

            int version   = jint(cached.model, "version", 0);
            int feat_dim  = jint(cached.model, "feat_dim", 0);
            double best_thr = jnum(cached.model, "best_thr", 0.5);
            int ma_len    = jint(cached.model, "ma_len", 12);
            double model_tp = jnum(cached.model, "tp", 0.02);
            double model_sl = jnum(cached.model, "sl", 0.01);

            if (version == 0 || feat_dim == 0) {
                json out{{"ok",false},{"error","model_incomplete"}};
                res.set_content(out.dump(), "application/json");
                return;
            }

            double atr     = atr14_from_M(cached.M15);
            double avg_atr = avg_atr_from_M(cached.M15);

            arma::mat M60_t, M240_t, M1440_t;
            const arma::mat *p60 = nullptr, *p240 = nullptr, *p1440 = nullptr;
            if (cached.M60.n_elem)   { M60_t   = cached.M60.t();   p60   = &M60_t;   }
            if (cached.M240.n_elem)  { M240_t  = cached.M240.t();  p240  = &M240_t;  }
            if (cached.M1440.n_elem) { M1440_t = cached.M1440.t(); p1440 = &M1440_t; }

            arma::mat raw15 = cached.M15.t();

            bool use_ppo_rl = cached.model.contains("actor_weights");
            json inf;
            bool policy_success = false;
            for (int attempt = 0; attempt < 3 && !policy_success; ++attempt) {
                try {
                    if (use_ppo_rl) {
                        inf = etai::ppo_rl_infer(raw15, cached.model);
                        if (jbool(inf, "ok", false)) {
                            inf["htf"] = json::object();
                            inf["htf"]["60"]   = json{{"agree", true}, {"score", 0.0}, {"strong", false}};
                            inf["htf"]["240"]  = json{{"agree", true}, {"score", 0.0}, {"strong", false}};
                            inf["htf"]["1440"] = json{{"agree", true}, {"score", 0.0}, {"strong", false}};
                        }
                    } else {
                        inf = etai::infer_with_policy_mtf(raw15, cached.model, p60, 12, p240, 12, p1440, 12);
                    }

                    if (jbool(inf, "ok", false) &&
                        jint(inf, "feat_dim_used", 0) > 0 &&
                        inf.contains("htf")) {
                        policy_success = true;
                    } else {
                        if (attempt < 2) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                } catch (...) {
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
            double score15  = jnum(inf, "score15", 0.0);

            LAST_INFER_TS.store((long long)time(nullptr)*1000, std::memory_order_relaxed);

            int upVotes=0, downVotes=0;
            htf_votes(inf, upVotes, downVotes);
            int netHTF = upVotes - downVotes;

            // =====================================================================
            // 🚀 НОВАЯ УМНАЯ ЛОГИКА РАСЧЕТА CONFIDENCE
            // =====================================================================

            // 1. Базовая уверенность от силы сигнала модели (независимо от порога!)
            double signal_strength = std::fabs(score15);
            double base_confidence = signal_strength * 100.0;  // 0.28 -> 28%

            // 2. Учитываем если сигнал сильнее обычного
            if (signal_strength > 0.3) {
                base_confidence += (signal_strength - 0.3) * 150.0;  // Бонус за силу >0.3
            }

            // 3. Бонус/штраф от HTF согласованности
            double htf_modifier = 0.0;
            if (netHTF != 0) {
                bool htf_agrees = (score15 > 0 && netHTF > 0) || (score15 < 0 && netHTF < 0);

                if (htf_agrees) {
                    htf_modifier = std::min(25.0, std::fabs(netHTF) * 8.0);
                } else {
                    // Штраф отключён, чтобы HTF не душил робота
                    htf_modifier = 0.0;
                }
            }

            double confidence = base_confidence + htf_modifier;
            confidence = std::max(0.0, std::min(100.0, confidence));

            // =====================================================================
            // 📊 VOLUME ANALYSIS - критично для скальпинга!
            // =====================================================================

            auto volume_signal = etai::analyze_volume(cached.M15, 20);

            std::cout << "[VOLUME] signal=" << volume_signal.signal
                      << " ratio=" << volume_signal.volume_ratio
                      << " OBV=" << volume_signal.obv
                      << " boost=" << volume_signal.confidence_boost << "%" << std::endl;

            if (sig == "LONG" && (volume_signal.signal == "strong_buy" || volume_signal.signal == "accumulation")) {
                std::cout << "[VOLUME] ✅ LONG confirmed by volume!" << std::endl;
            }
            else if (sig == "SHORT" && (volume_signal.signal == "strong_sell" || volume_signal.signal == "distribution")) {
                std::cout << "[VOLUME] ✅ SHORT confirmed by volume!" << std::endl;
            }
            else if (sig == "LONG" && volume_signal.signal == "strong_sell") {
                std::cout << "[VOLUME] ⚠️ WARNING: LONG but volume shows SELL pressure!" << std::endl;
            }
            else if (sig == "SHORT" && volume_signal.signal == "strong_buy") {
                std::cout << "[VOLUME] ⚠️ WARNING: SHORT but volume shows BUY pressure!" << std::endl;
            }
            else if (volume_signal.signal == "low_volume_warning") {
                std::cout << "[VOLUME] ⚠️ Low volume - suspicious move!" << std::endl;
            }

            // Дивергенция
            bool has_divergence = etai::detect_volume_divergence(cached.M15, 10);
            if (has_divergence) {
                std::cout << "[VOLUME] 🔥 DIVERGENCE detected - potential reversal!" << std::endl;
                if ((sig == "SHORT" && score15 < -0.2) || (sig == "LONG" && score15 > 0.2)) {
                    confidence += 12.0;
                }
            }

            confidence = std::max(0.0, std::min(100.0, confidence));

            // =====================================================================
            // 💰 OPEN INTEREST ANALYSIS
            // =====================================================================

            auto oi_data = etai::get_open_interest(symbol, "1h");

            if (oi_data.data_available) {
                double price_24h_ago = 0.0;
                if (cached.M15.n_cols >= 96) {
                    price_24h_ago = cached.M15(4, cached.M15.n_cols - 96);
                }
                double current_price = cached.M15(4, cached.M15.n_cols - 1);
                double price_change_24h = 0.0;
                if (price_24h_ago > 0) {
                    price_change_24h = ((current_price - price_24h_ago) / price_24h_ago) * 100.0;
                }

                double oi_boost = etai::analyze_oi_with_price(oi_data, price_change_24h, sig);
                confidence += oi_boost;

                if (oi_data.oi_change_percent > 100.0 && price_change_24h > 0) {
                    if (sig == "SHORT") {
                        confidence = std::max(0.0, confidence - 50.0);
                        std::cout << "[OI WARNING] 🚨 EXTREME PUMP but PPO says SHORT → reducing confidence by 50%" << std::endl;
                    } else if (sig == "LONG") {
                        confidence = std::min(confidence + 30.0, 100.0);
                        std::cout << "[OI CONFIRM] ✅ PUMP + LONG aligned → boosting confidence" << std::endl;
                    }
                } else if (oi_data.oi_change_percent < -100.0 && price_change_24h < 0) {
                    if (sig == "LONG") {
                        confidence = std::max(0.0, confidence - 50.0);
                        std::cout << "[OI WARNING] 🚨 EXTREME CRASH but PPO says LONG → reducing confidence by 50%" << std::endl;
                    } else if (sig == "SHORT") {
                        confidence = std::min(confidence + 30.0, 100.0);
                        std::cout << "[OI CONFIRM] ✅ CRASH + SHORT aligned → boosting confidence" << std::endl;
                    }
                }
                std::cout << "[OI] boost=" << oi_boost << "%" << std::endl;
            }

            confidence = std::max(0.0, std::min(100.0, confidence));

            // =====================================================================
            // 📊 SUPPORT/RESISTANCE ANALYSIS
            // =====================================================================

            auto sr_analysis = etai::analyze_support_resistance(cached.M15, 50);

            if (sr_analysis.position == "near_support" && sig == "LONG") {
                std::cout << "[S/R] LONG from support +" << sr_analysis.confidence_boost << "%" << std::endl;
            }
            else if (sr_analysis.position == "near_resistance" && sig == "SHORT") {
                std::cout << "[S/R] SHORT from resistance +" << std::abs(sr_analysis.confidence_boost) << "%" << std::endl;
            }
            else if (sr_analysis.position == "near_resistance" && sig == "LONG") {
                std::cout << "[S/R] LONG into resistance " << sr_analysis.confidence_boost << "%" << std::endl;
            }

            confidence = std::max(0.0, std::min(100.0, confidence));

            // =====================================================================
            // 💸 FUNDING RATE / ORDERBOOK ANALYSIS
            // =====================================================================

            auto orderbook = etai::get_order_book(symbol, 50);
            double orderbook_boost = 0.0;
            if (orderbook.data_available) {
                orderbook_boost = etai::apply_orderbook_boost(orderbook, sig);
                confidence += orderbook_boost;
            }

            auto funding_data = etai::get_funding_rate(symbol);

            if (funding_data.data_available) {
                double funding_boost = etai::apply_funding_boost(funding_data, sig);
                confidence += funding_boost;
                std::cout << "[FUNDING] boost=" << funding_boost << "%" << std::endl;
            }

            confidence = std::max(0.0, std::min(100.0, confidence));

            // =====================================================================
            // 🕯️ CANDLESTICK PATTERN ANALYSIS
            // =====================================================================

            auto candle_signal = etai::analyze_candles(cached.M15);

            if (candle_signal.pattern != etai::CandlePattern::NONE) {
                std::cout << "[CANDLE] Pattern: " << candle_signal.pattern_name
                          << " | Signal: " << candle_signal.signal
                          << " | Strength: " << candle_signal.strength
                          << " | Boost: " << candle_signal.confidence_boost << "%" << std::endl;

                if (sig == "LONG" && candle_signal.signal == "bullish") {
                    if (candle_signal.is_reversal) {
                        std::cout << "[CANDLE] 🔥 BULLISH REVERSAL pattern detected!" << std::endl;
                    }
                }
                else if (sig == "SHORT" && candle_signal.signal == "bearish") {
                    if (candle_signal.is_reversal) {
                        std::cout << "[CANDLE] 🔥 BEARISH REVERSAL pattern detected!" << std::endl;
                    }
                }
                else if (candle_signal.signal == "neutral") {
                    std::cout << "[CANDLE] ⚠️ Indecision - " << candle_signal.pattern_name << std::endl;
                }
                else if ((sig == "LONG" && candle_signal.signal == "bearish") ||
                         (sig == "SHORT" && candle_signal.signal == "bullish")) {
                    std::cout << "[CANDLE] ⚠️ Conflict: model says " << sig
                              << " but candle is " << candle_signal.signal << std::endl;
                }
            }

            confidence = std::max(0.0, std::min(100.0, confidence));

            // Логируем расчет
            std::cout << "[CONFIDENCE] signal_strength=" << signal_strength
                      << " base=" << base_confidence
                      << " netHTF=" << netHTF
                      << " htf_mod=" << htf_modifier
                      << " final=" << confidence << "%" << std::endl;

            // =====================================================================
            // 🎯 ОПРЕДЕЛЕНИЕ РЕЖИМА И АДАПТИВНОГО ПОРОГА
            // =====================================================================

            json safe_htf = (inf.contains("htf") && inf["htf"].is_object()) ? inf["htf"] : json::object();

            etai::MarketRegime regime = etai::detect_regime(safe_htf, cached.M15, atr, avg_atr);
            etai::RegimeParams params = etai::get_regime_params(regime);
            std::string regime_str = etai::regime_to_string(regime);

            std::string marketMode = "adaptive";
            if (score15 > 0.2)      marketMode = "bullish";
            else if (score15 < -0.2) marketMode = "bearish";
            else                    marketMode = "neutral";

            std::string original_sig = sig;
            double original_conf = confidence;

            double adaptive_threshold = params.min_confidence;

            if (regime == etai::MarketRegime::RANGE_BOUND && params.use_mean_reversion) {
                double channel_pos = etai::get_channel_position(cached.M15, atr);
                if ((channel_pos > 0.80 && sig == "SHORT") || (channel_pos < 0.20 && sig == "LONG")) {
                    adaptive_threshold = 15.0;
                    std::cout << "[RANGE] Good mean reversion setup, threshold lowered to 20%" << std::endl;
                } else if (channel_pos > 0.40 && channel_pos < 0.60) {
                    adaptive_threshold = 30.0;
                } else {
                    adaptive_threshold = 22.0;
                }
            } else if (regime == etai::MarketRegime::MANIPULATION) {
                sig = "NEUTRAL";
                confidence = 0.0;
                adaptive_threshold = 100.0;
            } else {
                bool signal_with_trend = false;
                bool signal_against_trend = false;

                if (sig == "LONG") {
                    signal_with_trend    = (netHTF > 0);
                    signal_against_trend = (netHTF < -1);
                } else if (sig == "SHORT") {
                    signal_with_trend    = (netHTF < 0);
                    signal_against_trend = (netHTF > 1);
                }

                if (signal_with_trend) {
                    adaptive_threshold = params.min_confidence * 0.5;
                    std::cout << "[TREND] Signal with trend, threshold=" << adaptive_threshold << "%" << std::endl;
                } else if (signal_against_trend) {
                    adaptive_threshold = params.min_confidence * 1.2;
                    std::cout << "[COUNTER] Signal against trend, threshold=" << adaptive_threshold << "%" << std::endl;
                } else {
                    adaptive_threshold = params.min_confidence;
                }
            }

            // Калиброванный порог по фактической истории робота
            double calibrated_threshold = etai::g_calibrator.get_calibrated_threshold(adaptive_threshold);

            // =====================================================================
            // ✅ ФИНАЛЬНОЕ РЕШЕНИЕ
            // =====================================================================

            if (confidence < calibrated_threshold) {
                sig = "NEUTRAL";
                std::cout << "[FILTER] Confidence " << confidence
                          << "% below calibrated threshold " << calibrated_threshold
                          << "% (base=" << adaptive_threshold << "%), signal blocked" << std::endl;
            }

            // ИСКЛЮЧЕНИЕ: Если модель ОЧЕНЬ уверена (>70%) - торгуем в любом случае!
            if (confidence >= 70.0 && sig == "NEUTRAL" && original_sig != "NEUTRAL") {
                sig = original_sig;
                std::cout << "[OVERRIDE] High confidence " << confidence
                          << "% overrides filters, signal: " << sig << std::endl;
            }

            if (sig != original_sig || std::fabs(confidence - original_conf) > 2.0) {
                std::cerr << "[DECISION] Regime=" << regime_str
                          << " | Original: " << original_sig << " @ " << (int)original_conf << "%"
                          << " | Final: " << sig << " @ " << (int)confidence << "%"
                          << " | Threshold: " << (int)calibrated_threshold
                          << "% (base " << (int)adaptive_threshold << "%)" << std::endl;
            }

            if (sig == "LONG")      INFER_SIG_LONG.fetch_add(1, std::memory_order_relaxed);
            else if (sig == "SHORT")INFER_SIG_SHORT.fetch_add(1, std::memory_order_relaxed);
            else                    INFER_SIG_NEUTRAL.fetch_add(1, std::memory_order_relaxed);

            double final_tp = params.tp_percent;
            double final_sl = params.sl_percent;

            if (regime == etai::MarketRegime::BREAKOUT_UP || regime == etai::MarketRegime::BREAKOUT_DOWN) {
                double atr_percent = (atr / (double)cached.M15.row(4)(cached.M15.n_cols-1));
                double multiplier = std::min(atr_percent / 0.02, 3.0);
                final_tp = params.tp_percent * multiplier;
                final_sl = params.sl_percent * multiplier;
            }

            json out{
                {"ok", true},
                {"volume_signal", volume_signal.signal},
                {"volume_ratio", volume_signal.volume_ratio},
                {"volume_obv", volume_signal.obv},
                {"volume_boost", volume_signal.confidence_boost},
                {"oi_available", oi_data.data_available},
                {"oi_current", oi_data.open_interest},
                {"oi_change_24h", oi_data.oi_change_percent},
                {"oi_signal", oi_data.signal},
                {"sr_position", sr_analysis.position},
                {"sr_support", sr_analysis.nearest_support},
                {"sr_resistance", sr_analysis.nearest_resistance},
                {"funding_available", funding_data.data_available},
                {"orderbook_available", orderbook.data_available},
                {"orderbook_imbalance", orderbook.imbalance},
                {"orderbook_signal", orderbook.signal},
                {"orderbook_boost", orderbook_boost},
                {"funding_rate", funding_data.funding_rate},
                {"funding_signal", funding_data.signal},
                {"candle_pattern", candle_signal.pattern_name},
                {"candle_signal", candle_signal.signal},
                {"candle_strength", candle_signal.strength},
                {"candle_boost", candle_signal.confidence_boost},
                {"mode", "smart_aggressive"},
                {"regime", regime_str},
                {"regime_note", params.note},
                {"symbol", symbol},
                {"interval", interval},
                {"version", version},
                {"thr", best_thr},
                {"ma_len", ma_len},
                {"signal", sig},
                {"score15", score15},
                {"long_prob", jnum(inf, "long_prob", 0.0)},
                {"short_prob", jnum(inf, "short_prob", 0.0)},
                {"market_mode", marketMode},
                {"confidence", confidence},
                {"adaptive_threshold", adaptive_threshold},
                {"calibrated_threshold", calibrated_threshold},
                {"netHTF", netHTF},
                {"htf", safe_htf},
                {"feat_dim_used", jint(inf, "feat_dim_used", 0)},
                {"used_norm", jbool(inf, "used_norm", true)},
                {"wctx_htf", jnum(inf, "wctx_htf", 0.0)},
                {"vol_threshold", jnum(inf, "vol_threshold", 0.0)},
                {"agents", make_agents_summary()},
                {"from_cache", from_cache},
                {"atr", atr},
                {"avg_atr", avg_atr},
                {"tp", final_tp},
                {"sl", final_sl},
                {"model_base_tp", model_tp},
                {"model_base_sl", model_sl}
            };

            if (cached.M15.n_rows >= 5 && cached.M15.n_cols >= 1) {
                double last = (double)cached.M15.row(4)(cached.M15.n_cols - 1);
                out["last_close"]     = last;
                out["tp_price_long"]  = last * (1.0 + final_tp);
                out["sl_price_long"]  = last * (1.0 - final_sl);
                out["tp_price_short"] = last * (1.0 - final_tp);
                out["sl_price_short"] = last * (1.0 + final_sl);

                if (regime == etai::MarketRegime::RANGE_BOUND) {
                    out["channel_position"] = etai::get_channel_position(cached.M15, atr);
                }
            }

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
