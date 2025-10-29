#pragma once
#include "server_utils.h"

static void register_metrics_route(httplib::Server& srv){
  srv.Get("/metrics", [](const httplib::Request&, httplib::Response& res){
    REQ_METRICS.fetch_add(1, std::memory_order_relaxed);
    long long now_ms = (long long)time(nullptr)*1000;
    long long uptime_sec = (now_ms - PROCESS_START_MS) / 1000;

    std::ostringstream m;
    m << "# HELP edge_up 1 if server is up\n# TYPE edge_up gauge\nedge_up 1\n";
    m << "# HELP edge_uptime_seconds Process uptime\n# TYPE edge_uptime_seconds counter\nedge_uptime_seconds " << uptime_sec << "\n";

    m << "# HELP edge_requests_total Total HTTP requests by route\n# TYPE edge_requests_total counter\n";
    m << "edge_requests_total{route=\"/health\"} "    << REQ_HEALTH.load()   << "\n";
    m << "edge_requests_total{route=\"/api/backfill\"} " << REQ_BACKFILL.load() << "\n";
    m << "edge_requests_total{route=\"/api/train\"} "    << REQ_TRAIN.load()    << "\n";
    m << "edge_requests_total{route=\"/api/model\"} "    << REQ_MODEL.load()    << "\n";
    m << "edge_requests_total{route=\"/api/infer\"} "    << REQ_INFER.load()    << "\n";
    m << "edge_requests_total{route=\"/metrics\"} "      << REQ_METRICS.load()  << "\n";

    m << "# HELP edge_trains_total Successful train operations\n# TYPE edge_trains_total counter\n";
    m << "edge_trains_total " << TRAINS_TOTAL.load() << "\n";

    m << "# HELP edge_last_train_ts_ms Last train timestamp in ms\n# TYPE edge_last_train_ts_ms gauge\n";
    m << "edge_last_train_ts_ms " << LAST_TRAIN_TS.load() << "\n";

    m << "# HELP edge_infer_signals_total Inference signals count by label\n# TYPE edge_infer_signals_total counter\n";
    m << "edge_infer_signals_total{signal=\"LONG\"} "    << INFER_SIG_LONG.load()    << "\n";
    m << "edge_infer_signals_total{signal=\"SHORT\"} "   << INFER_SIG_SHORT.load()   << "\n";
    m << "edge_infer_signals_total{signal=\"NEUTRAL\"} " << INFER_SIG_NEUTRAL.load() << "\n";

    m << "# HELP edge_data_rows Number of cached data rows by TF\n# TYPE edge_data_rows gauge\n";
    m << "edge_data_rows{tf=\"15\"} "    << DATA_ROWS_15.load()    << "\n";
    m << "edge_data_rows{tf=\"60\"} "    << DATA_ROWS_60.load()    << "\n";
    m << "edge_data_rows{tf=\"240\"} "   << DATA_ROWS_240.load()   << "\n";
    m << "edge_data_rows{tf=\"1440\"} "  << DATA_ROWS_1440.load()  << "\n";

    m << "# HELP edge_train_rows_used Effective training rows used\n# TYPE edge_train_rows_used gauge\n";
    m << "edge_train_rows_used " << TRAIN_ROWS_USED.load() << "\n";

    m << "# HELP edge_model_build_ts_ms Last model build ts\n# TYPE edge_model_build_ts_ms gauge\n";
    m << "edge_model_build_ts_ms " << MODEL_BUILD_TS.load() << "\n";

    m << "# HELP edge_infer_last_ts_ms Last infer served ts\n# TYPE edge_infer_last_ts_ms gauge\n";
    m << "edge_infer_last_ts_ms " << LAST_INFER_TS.load() << "\n";

    // CV
    m << "# HELP edge_cv_folds Number of CV folds used in last training\n# TYPE edge_cv_folds gauge\n";
    m << "edge_cv_folds " << CV_FOLDS.load() << "\n";

    m << "# HELP edge_cv_effective_folds Effective CV folds after data/warmup constraints\n# TYPE edge_cv_effective_folds gauge\n";
    m << "edge_cv_effective_folds " << CV_EFFECTIVE_FOLDS.load() << "\n";

    m << "# HELP edge_cv_is_sharpe In-sample Sharpe across CV folds (avg)\n# TYPE edge_cv_is_sharpe gauge\n";
    m << "edge_cv_is_sharpe " << CV_IS_SHARPE.load() << "\n";

    m << "# HELP edge_cv_oos_sharpe Out-of-sample Sharpe across CV folds (avg)\n# TYPE edge_cv_oos_sharpe gauge\n";
    m << "edge_cv_oos_sharpe " << CV_OOS_SHARPE.load() << "\n";

    m << "# HELP edge_cv_is_expectancy In-sample expectancy across CV folds (avg)\n# TYPE edge_cv_is_expectancy gauge\n";
    m << "edge_cv_is_expectancy " << CV_IS_EXPEC.load() << "\n";

    m << "# HELP edge_cv_oos_expectancy Out-of-sample expectancy across CV folds (avg)\n# TYPE edge_cv_oos_expectancy gauge\n";
    m << "edge_cv_oos_expectancy " << CV_OOS_EXPEC.load() << "\n";

    m << "# HELP edge_cv_oos_drawdown_max Max OOS drawdown across CV folds\n# TYPE edge_cv_oos_drawdown_max gauge\n";
    m << "edge_cv_oos_drawdown_max " << CV_OOS_DD_MAX.load() << "\n";

    // Модельные агрегаты
    m << "# HELP edge_model_thr Best threshold from last model\n# TYPE edge_model_thr gauge\n";
    m << "edge_model_thr " << MODEL_BEST_THR.load() << "\n";
    m << "# HELP edge_model_ma_len Model MA length\n# TYPE edge_model_ma_len gauge\n";
    m << "edge_model_ma_len " << MODEL_MA_LEN.load() << "\n";

    // Свежесть данных
    long long fresh_ms = DATA_FRESH_MS.load();
    m << "# HELP edge_data_fresh_ms Data freshness (ms since last 15m bar)\n# TYPE edge_data_fresh_ms gauge\n";
    m << "edge_data_fresh_ms " << (fresh_ms<0?0:fresh_ms) << "\n";
    m << "# HELP edge_data_fresh_minutes Data freshness (minutes)\n# TYPE edge_data_fresh_minutes gauge\n";
    m << "edge_data_fresh_minutes " << (fresh_ms>0? (fresh_ms/60000.0) : 0.0) << "\n";

    res.set_content(m.str(), "text/plain; version=0.0.4");
  });
}
