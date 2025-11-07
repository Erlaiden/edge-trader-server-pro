#include "../httplib.h"
#include "../server_accessors.h"
#include "json.hpp"
#include <fstream>
#include <sstream>
#include <string>
#include <limits>
#include <cmath>

using json = nlohmann::json;

// Безопасное чтение JSON с диска
static inline json read_json_file(const std::string& path, std::string& err) {
    try {
        std::ifstream f(path);
        if (!f) { err = "cannot open file: " + path; return json::object(); }
        json j; f >> j;
        if (!j.is_object()) { err = "json is not object"; return json::object(); }
        return j;
    } catch (const std::exception& ex) {
        err = std::string("json parse failed: ") + ex.what();
        return json::object();
    } catch (...) {
        err = "unknown error while parsing json";
        return json::object();
    }
}

// Безопасная запись JSON на диск (с pretty)
static inline bool write_json_file(const std::string& path, const json& j, std::string& err) {
    try {
        std::ofstream f(path, std::ios::trunc);
        if (!f) { err = "cannot open for write: " + path; return false; }
        f << j.dump(2);
        return true;
    } catch (const std::exception& ex) {
        err = std::string("write failed: ") + ex.what();
        return false;
    } catch (...) {
        err = "unknown error while writing json";
        return false;
    }
}

// Валидация чисел
static inline bool finite_in(double v, double lo, double hi) {
    return std::isfinite(v) && v >= lo && v <= hi;
}

namespace etai {

inline void register_model_set_routes(httplib::Server& svr) {
    // POST /api/model/set
    // Body JSON (всё опционально):
    // { "thr":0.36, "ma_len":12, "feat_dim":32, "tp":0.0044, "sl":0.0016, "path":"/opt/.../cache/models/BTCUSDT_15_ppo_pro.json" }
    //
    // Логика:
    // 1) Читаем текущую модель из атомиков/диска
    // 2) Применяем присланные поля
    // 3) Обновляем атомики (thr/ma/feat_dim)
    // 4) Перезаписываем JSON на диск (best_thr/ma_len/tp/sl и policy.feat_dim)
    // 5) Кладём обновлённый JSON в set_current_model(...)
    svr.Post("/api/model/set", [](const httplib::Request& req, httplib::Response& res) {
        json in;
        try {
            in = json::parse(req.body.empty() ? "{}" : req.body);
            if (!in.is_object()) throw std::runtime_error("body is not json object");
        } catch (const std::exception& ex) {
            json r; r["ok"] = false; r["error"] = std::string("invalid json: ") + ex.what();
            res.status = 400; res.set_content(r.dump(2), "application/json"); return;
        }

        // Путь к модели (дефолт)
        std::string path = "/opt/edge-trader-server/cache/models/BTCUSDT_15_ppo_pro.json";
        if (in.contains("path") && in["path"].is_string()) {
            path = in["path"].get<std::string>();
        }

        // Считать базовый JSON из set_current_model(), если пуст — с диска
        json base = etai::get_current_model();
        if (!base.is_object() || base.empty()) {
            std::string rerr;
            base = read_json_file(path, rerr);
            if (!rerr.empty()) {
                // если не смогли прочитать — начнём с пустого объекта
                base = json::object();
            }
        }

        // Значения к применению (с дефолтами из атомиков)
        double thr = etai::get_model_thr();
        long long ma = etai::get_model_ma_len();
        int feat = 0;

        // Присланные поля (всё опционально)
        bool has_thr = false, has_ma = false, has_feat = false, has_tp = false, has_sl = false;
        double tp = std::numeric_limits<double>::quiet_NaN();
        double sl = std::numeric_limits<double>::quiet_NaN();

        if (in.contains("thr") && (in["thr"].is_number_float() || in["thr"].is_number())) {
            double v = in["thr"].get<double>();
            if (!finite_in(v, 1e-6, 1.0)) {
                json r; r["ok"] = false; r["error"] = "thr out of range (0..1)"; res.status = 400; res.set_content(r.dump(2),"application/json"); return;
            }
            thr = v; has_thr = true;
        }
        if (in.contains("ma_len") && in["ma_len"].is_number_integer()) {
            long long v = in["ma_len"].get<long long>();
            if (v <= 0 || v > 100000) {
                json r; r["ok"] = false; r["error"] = "ma_len out of range (>0)"; res.status = 400; res.set_content(r.dump(2),"application/json"); return;
            }
            ma = v; has_ma = true;
        }
        if (in.contains("feat_dim") && in["feat_dim"].is_number_integer()) {
            int v = in["feat_dim"].get<int>();
            if (v <= 0 || v >= 4096) {
                json r; r["ok"] = false; r["error"] = "feat_dim out of range (1..4095)"; res.status = 400; res.set_content(r.dump(2),"application/json"); return;
            }
            feat = v; has_feat = true;
        }
        if (in.contains("tp") && (in["tp"].is_number_float() || in["tp"].is_number())) {
            tp = in["tp"].get<double>();
            if (!finite_in(tp, 0.0, 1.0)) {
                json r; r["ok"] = false; r["error"] = "tp out of range (0..1)"; res.status = 400; res.set_content(r.dump(2),"application/json"); return;
            }
            has_tp = true;
        }
        if (in.contains("sl") && (in["sl"].is_number_float() || in["sl"].is_number())) {
            sl = in["sl"].get<double>();
            if (!finite_in(sl, 0.0, 1.0)) {
                json r; r["ok"] = false; r["error"] = "sl out of range (0..1)"; res.status = 400; res.set_content(r.dump(2),"application/json"); return;
            }
            has_sl = true;
        }

        // Обновляем атомики
        if (has_thr) etai::set_model_thr(thr);
        if (has_ma)  etai::set_model_ma_len(ma);
        if (has_feat && feat > 0) etai::set_feat_dim(feat);

        // Подготовим структуру в JSON
        if (!base.is_object()) base = json::object();
        if (has_thr) base["best_thr"] = thr;
        if (has_ma)  base["ma_len"]   = ma;
        if (has_tp)  base["tp"]       = tp;
        if (has_sl)  base["sl"]       = sl;

        // policy.feat_dim
        if (has_feat) {
            if (!base.contains("policy") || !base["policy"].is_object()) base["policy"] = json::object();
            base["policy"]["feat_dim"] = feat;
        }

        // Пишем на диск
        std::string werr;
        if (!write_json_file(path, base, werr)) {
            json r; r["ok"] = false; r["error"] = werr; r["path"] = path;
            res.status = 500; res.set_content(r.dump(2), "application/json"); return;
        }

        // Обновляем текущую модель в памяти
        try { etai::set_current_model(base); } catch (...) {}

        // Ответ
        json r;
        r["ok"] = true;
        r["applied"] = json::object();
        if (has_thr)  r["applied"]["thr"]      = thr;
        if (has_ma)   r["applied"]["ma_len"]   = ma;
        if (has_feat) r["applied"]["feat_dim"] = feat;
        if (has_tp)   r["applied"]["tp"]       = tp;
        if (has_sl)   r["applied"]["sl"]       = sl;
        r["path"] = path;

        // Текущее состояние атомиков для удобства
        r["state"] = {
            {"model_thr",       etai::get_model_thr()},
            {"model_ma_len",    etai::get_model_ma_len()},
            {"model_feat_dim",  etai::get_model_feat_dim()}
        };

        res.status = 200;
        res.set_content(r.dump(2), "application/json");
    });
}

} // namespace etai

// Глобальный адаптер
inline void register_model_set_routes(httplib::Server& svr) {
    etai::register_model_set_routes(svr);
}
