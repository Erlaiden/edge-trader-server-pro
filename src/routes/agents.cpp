#include <httplib.h>
#include <armadillo>
#include <deque>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

#include "../json.hpp"
#include "../server_accessors.h"   // etai::{get_current_model}
#include "../infer_policy.h"       // etai::infer_with_policy (ожидает raw OHLCV матрицу)
                                    // features/build_feature_matrix уже подтянется внутри

// Цель: убрать заглушки агентов и опираться на реальный инференс модели.
// Эндпоинт: /api/agents/test?type=long|short|flat|correction|breakout&thr=0.5&symbol=BTCUSDT&interval=15

using json = nlohmann::json;

namespace etai {

// --- утилиты ---

static inline std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::toupper(c); });
    return s;
}

// читаем последние N строк из cache/<SYMBOL>_<INTERVAL>.csv
// ожидаем CSV c минимум 6 колонками: ts,open,high,low,close,volume
// если колонок 7 (есть turnover) — берём первые 6.
static bool load_raw_ohlcv_lastN(const std::string& symbol,
                                 const std::string& interval,
                                 size_t lastN,
                                 arma::Mat<double>& out_raw)
{
    const std::string path = "cache/" + upper(symbol) + "_" + interval + ".csv";
    std::ifstream f(path);
    if (!f.good()) return false;

    std::deque<std::array<double,6>> ring;
    ring.resize(0);

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::array<double,6> row6{};
        // быстрый парсер: берём первые 6 чисел, остальное игнорируем
        std::istringstream iss(line);
        std::string tok;
        int col = 0;
        bool ok = true;
        while (std::getline(iss, tok, ',')) {
            if (col < 6) {
                try {
                    // ts может быть очень длинным — парсим как double (в матрице всё равно double)
                    row6[col] = std::stod(tok);
                } catch (...) { ok = false; break; }
            }
            ++col;
        }
        if (!ok || col < 6) continue;

        ring.push_back(row6);
        if (lastN && ring.size() > lastN) ring.pop_front();
    }
    if (ring.empty()) return false;

    const size_t n = ring.size();
    out_raw.set_size(n, 6);
    for (size_t i=0; i<n; ++i) {
        for (int j=0; j<6; ++j) out_raw(i,j) = ring[i][j];
    }
    return true;
}

// --- роуты агентов (без заглушек) ---

void setup_agents_routes(httplib::Server& svr) {
    // /api/agents/test?type=...&thr=0.4&symbol=BTCUSDT&interval=15
    svr.Get("/api/agents/test", [](const httplib::Request& req, httplib::Response& res) {
        const std::string type     = req.has_param("type")     ? req.get_param_value("type")     : "breakout";
        const std::string symbol   = req.has_param("symbol")   ? req.get_param_value("symbol")   : "BTCUSDT";
        const std::string interval = req.has_param("interval") ? req.get_param_value("interval") : "15";
        double thr = 0.5;
        if (req.has_param("thr")) {
            try { thr = std::stod(req.get_param_value("thr")); } catch (...) {}
        }
        if (thr < 0.0) thr = 0.0;
        if (thr > 0.99) thr = 0.99;

        // 1) Загружаем последние бары (хватает 300–600 для фич v9)
        arma::Mat<double> raw15;
        const bool ok_raw = load_raw_ohlcv_lastN(symbol, interval, /*lastN*/ 600, raw15);
        if (!ok_raw || raw15.n_rows < 300 || raw15.n_cols < 6) {
            json out = {
                {"ok", false},
                {"error", "raw_not_available_or_short"},
                {"symbol", symbol},
                {"interval", interval}
            };
            res.set_content(out.dump(2), "application/json");
            return;
        }

        // 2) Текущая модель (из атомиков/диска)
        const json model = etai::get_current_model();
        if (!model.is_object() || !model.contains("policy")) {
            json out = {
                {"ok", false},
                {"error", "model_not_ready"},
                {"note",  "train the model via /api/train first"}
            };
            res.set_content(out.dump(2), "application/json");
            return;
        }

        // 3) Реальный инференс
        json inf = etai::infer_with_policy(raw15, model);
        if (!inf.value("ok", false)) {
            json out = {
                {"ok", false},
                {"error", "infer_failed"},
                {"details", inf}
            };
            res.set_content(out.dump(2), "application/json");
            return;
        }

        // score в [-1,1], confidence = |score|
        const double score = inf.value("score", 0.0);
        double confidence = std::abs(score);
        if (!std::isfinite(confidence)) confidence = 0.0;
        if (confidence > 1.0) confidence = 1.0;

        int decision = 0;
        if (score >=  thr) decision =  1;     // LONG
        else if (score <= -thr) decision = -1; // SHORT
        else decision = 0;                     // FLAT

        json out = {
            {"ok", true},
            {"agent", type},
            {"symbol", upper(symbol)},
            {"interval", interval},
            {"thr", thr},
            {"decision", decision},
            {"confidence", confidence},
            {"score", score},
            {"infer", {
                {"signal", inf.value("signal","NEUTRAL")},
                {"sigma",  inf.value("sigma", 0.0)},
                {"vol_threshold", inf.value("vol_threshold", 0.0)}
            }}
        };
        res.set_content(out.dump(2), "application/json");
    });
}

} // namespace etai
