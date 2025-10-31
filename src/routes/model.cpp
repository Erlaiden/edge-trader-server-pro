// Файл может инклюдиться прямо из main.cpp, поэтому без внешних "routes.h".
#include "../httplib.h"
#include "../server_accessors.h"
#include "json.hpp"
#include <fstream>
#include <sstream>
#include <string>

using json = nlohmann::json;

namespace etai {

static json load_json_file(const std::string& path, std::string& err) {
    std::ifstream f(path);
    if (!f) { err = "cannot open file: " + path; return {}; }
    std::ostringstream ss; ss << f.rdbuf();
    try {
        return json::parse(ss.str());
    } catch (const std::exception& ex) {
        err = std::string("json parse failed: ") + ex.what();
        return {};
    }
}

// Основная функция, которую вызывает main.cpp
inline void register_model_routes(httplib::Server& svr) {
    // Текущее состояние модели (атомики)
    svr.Get("/api/model", [](const httplib::Request&, httplib::Response& res) {
        json r;
        r["ok"] = true;
        r["model_thr"]      = etai::get_model_thr();
        r["model_ma_len"]   = etai::get_model_ma_len();
        // Если в проекте есть геттер размерности — отдадим
        try { r["model_feat_dim"] = etai::get_model_feat_dim(); } catch (...) {}
        res.status = 200;
        res.set_content(r.dump(2), "application/json");
    });

    // Горячая подстановка модели с диска в память сервера
    // Пример:
    //   /api/model/reload
    //   /api/model/reload?path=/opt/edge-trader-server/cache/models/BTCUSDT_15_ppo_pro.json
    svr.Get("/api/model/reload", [](const httplib::Request& req, httplib::Response& res) {
        const std::string path = req.has_param("path")
            ? req.get_param_value("path")
            : std::string("/opt/edge-trader-server/cache/models/BTCUSDT_15_ppo_pro.json");

        std::string err;
        json j = load_json_file(path, err);
        if (!err.empty()) {
            json r; r["ok"] = false; r["error"] = err; r["path"] = path;
            res.status = 400;
            res.set_content(r.dump(2), "application/json");
            return;
        }

        // Безопасно извлекаем поля (с дефолтами из текущих атомиков)
        double thr = etai::get_model_thr();
        int ma     = etai::get_model_ma_len();
        int feat   = 0;

        try { if (j.contains("best_thr") && !j["best_thr"].is_null()) thr = j["best_thr"].get<double>(); } catch (...) {}
        try { if (j.contains("ma_len")   && !j["ma_len"].is_null())   ma  = j["ma_len"].get<int>();      } catch (...) {}
        try {
            if (j.contains("policy") && j["policy"].contains("feat_dim") && !j["policy"]["feat_dim"].is_null())
                feat = j["policy"]["feat_dim"].get<int>();
        } catch (...) {}

        // Применяем к атомикам
        etai::set_model_thr(thr);
        etai::set_model_ma_len(ma);
        try { if (feat > 0) etai::set_model_feat_dim(feat); } catch (...) {}
        try { etai::set_current_model(j.dump()); } catch (...) {}

        json r;
        r["ok"] = true;
        r["applied"] = { {"thr", thr}, {"ma_len", ma}, {"feat_dim", feat} };
        r["path"] = path;
        res.status = 200;
        res.set_content(r.dump(2), "application/json");
    });
}

// Совместимость: если где-то вызывается старое имя
inline void register_model_route(httplib::Server& svr) {
    register_model_routes(svr);
}

} // namespace etai
