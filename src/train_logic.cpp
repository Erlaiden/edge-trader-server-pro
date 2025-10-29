#include "train_logic.h"
#include "json.hpp"
#include "ppo_pro.h"
#include "utils.h"
#include "rt_metrics.h"
#include "features/features.h"   // фичи для policy
#include <armadillo>
#include <filesystem>
#include <fstream>
#include <ctime>

using json = nlohmann::json;
namespace fs = std::filesystem;

// простая нормализация строк (z-score по каждой строке)
static arma::mat zscore_rows(const arma::mat& X) {
    arma::mat Z = X;
    for (size_t r=0; r<Z.n_rows; ++r) {
        arma::rowvec row = Z.row(r);
        double mean = arma::mean(row);
        double sd   = arma::stddev(row);
        if (sd < 1e-12) sd = 1.0;
        Z.row(r) = (row - mean) / sd;
    }
    return Z;
}

nlohmann::json run_train_pro_and_save(const std::string& symbol,
                                      const std::string& interval,
                                      int episodes, double tp, double sl, int ma_len)
{
    arma::mat M15 = etai::load_cached_matrix(symbol, interval);
    if (M15.n_elem == 0) {
        return json{{"ok",false},{"error","no_cached_data"},{"hint","call /api/backfill first"}};
    }

    arma::mat M60   = etai::load_cached_matrix(symbol, "60");
    arma::mat M240  = etai::load_cached_matrix(symbol, "240");
    arma::mat M1440 = etai::load_cached_matrix(symbol, "1440");

    const arma::mat* p60   = M60.n_elem   ? &M60   : nullptr;
    const arma::mat* p240  = M240.n_elem  ? &M240  : nullptr;
    const arma::mat* p1440 = M1440.n_elem ? &M1440 : nullptr;

    json metrics = etai::trainPPO_pro(M15, p60, p240, p1440, episodes, tp, sl, ma_len);
    metrics["version"] = metrics.value("version", 3);
    metrics["schema"]  = metrics.value("schema",  std::string("ppo_pro_v1"));

    json out{
        {"ok", metrics.value("ok", false)},
        {"symbol", symbol},
        {"interval", interval},
        {"episodes", episodes},
        {"tp", tp},
        {"sl", sl},
        {"metrics", metrics}
    };

    if (out["ok"].get<bool>()) {
        fs::create_directories("cache/models");
        long long now_ms = (long long)time(nullptr)*1000;

        // Метрики в атомики для /metrics
        const auto cv_folds  = (unsigned long long)metrics.value("cv_folds", 0);
        const auto cv_eff    = (unsigned long long)metrics.value("cv_effective_folds", 0);
        const auto is_sum    = metrics.value("is_summary", json::object());
        const auto oos_sum   = metrics.value("oos_summary", json::object());

        CV_FOLDS.store(cv_folds, std::memory_order_relaxed);
        CV_EFFECTIVE_FOLDS.store(cv_eff, std::memory_order_relaxed);
        CV_IS_SHARPE.store(is_sum.value("sharpe", 0.0), std::memory_order_relaxed);
        CV_OOS_SHARPE.store(oos_sum.value("sharpe", 0.0), std::memory_order_relaxed);
        CV_IS_EXPEC.store(is_sum.value("expectancy", 0.0), std::memory_order_relaxed);
        CV_OOS_EXPEC.store(oos_sum.value("expectancy", 0.0), std::memory_order_relaxed);
        CV_OOS_DD_MAX.store(oos_sum.value("drawdown_max", 0.0), std::memory_order_relaxed);

        // ---------- Сборка модели ----------
        json model{
            {"ok", true},
            {"ts", now_ms},
            {"symbol", symbol},
            {"interval", interval},
            {"mode", "pro"},
            {"version", metrics.value("version", 3)},
            {"schema",  metrics.value("schema",  "ppo_pro_v1")},
            {"build_ts", metrics.value("build_ts", now_ms)},
            {"ma_len", metrics.value("ma_len", ma_len)},
            {"best_thr", metrics.value("best_thr", 0.0)},
            {"tp", metrics.value("tp", 0.0)},
            {"sl", metrics.value("sl", 0.0)},
            {"episodes", metrics.value("episodes", episodes)},
            {"log_path", metrics.value("log_path","")},
            {"train_rows_total", metrics.value("train_rows_total", 0)},
            {"warmup_bars", metrics.value("warmup_bars", 0)},
            {"train_rows_used", metrics.value("train_rows_used", 0)},
            {"data_time_range", metrics.value("data_time_range", json::object())},
            {"split_index", metrics.value("split_index", 0)},
            {"cv_folds", metrics.value("cv_folds", 0)},
            {"cv_effective_folds", metrics.value("cv_effective_folds", 0)},
            {"is", metrics.contains("is") ? metrics["is"] : metrics.value("is_summary", json::object())},
            {"oos", metrics.contains("oos") ? metrics["oos"] : metrics.value("oos_summary", json::object())}
        };

        // ---------- Policy: берём из metrics если есть, иначе — эвристика ----------
        bool attached_policy = false;
        try {
            if (metrics.contains("policy")
                && metrics["policy"].is_object()
                && metrics["policy"].contains("W")
                && metrics["policy"].contains("b")
                && metrics["policy"].contains("feat_dim")) {
                model["policy"] = metrics["policy"];
                model["policy_source"] = "learn";
                if (!model["policy"].contains("note")) {
                    model["policy"]["note"] = "learned_from_train";
                }
                attached_policy = true;
            }
        } catch (...) {
            attached_policy = false;
        }

        if (!attached_policy) {
            // Эвристика только если нет выученной policy
            try {
                arma::mat X = etai::build_feature_matrix(M15); // ожидаем D x N
                if (X.n_rows >= 1 && X.n_cols >= 2) {
                    X = zscore_rows(X);
                    int D = (int)X.n_rows;
                    std::vector<double> W((size_t)D, (D > 0 ? (1.0 / (double)D) : 0.0));
                    std::vector<double> b(1, 0.0);
                    json policy{
                        {"feat_dim", D},
                        {"W", W},
                        {"b", b},
                        {"note", "heuristic policy; to be replaced by learned weights"}
                    };
                    model["policy"] = policy;
                    model["policy_source"] = "heuristic";
                }
            } catch (...) {
                // policy опционален — если что-то пошло не так, просто пропускаем
            }
        }

        const std::string path = "cache/models/" + symbol + "_" + interval + "_ppo_pro.json";
        std::ofstream f(path);
        if (f) f << model.dump(2);
        out["model_path"] = path;

        TRAINS_TOTAL.fetch_add(1, std::memory_order_relaxed);
        LAST_TRAIN_TS.store(now_ms, std::memory_order_relaxed);
        TRAIN_ROWS_USED.store(model.value("train_rows_used", 0), std::memory_order_relaxed);
        MODEL_BUILD_TS.store(model.value("build_ts", now_ms), std::memory_order_relaxed);
        MODEL_BEST_THR.store(model.value("best_thr", 0.0), std::memory_order_relaxed);
        MODEL_MA_LEN.store(model.value("ma_len", 0), std::memory_order_relaxed);
    }

    return out;
}
