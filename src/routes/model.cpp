// routes/model.cpp — fixed version with full model reflection

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

// ----------- MAIN -----------
inline void register_model_routes(httplib::Server& svr) {

    // 1) Отдача состояния модели (атомики + JSON)
    svr.Get("/api/model", [](const httplib::Request&, httplib::Response& res) {
        json r;
        r["ok"] = true;
        r["model_thr"]    = etai::get_model_thr();
        r["model_ma_len"] = etai::get_model_ma_len();
        try { r["model_feat_dim"] = etai::get_model_feat_dim(); } catch (...) {}

        // Дополнительно: извлечь ключевые поля из current_model
        try {
            json m = etai::get_current_model();
            json mshort;
            if (m.contains("best_thr")) mshort["best_thr"] = m["best_thr"];
            if (m.contains("tp"))       mshort["tp"]       = m["tp"];
            if (m.contains("sl"))       mshort["sl"]       = m["sl"];
            if (m.contains("ma_len"))   mshort["ma_len"]   = m["ma_len"];
            if (m.contains("policy") && m["policy"].contains("feat_dim"))
                mshort["feat_dim"] = m["policy"]["feat_dim"];
            if (!mshort.empty()) r["model"] = mshort;
        } catch (...) {}

        res.status = 200;
        res.set_content(r.dump(2), "application/json");
    });

    // 2) Горячая подстановка модели
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

        // --- defaults from current ---
        double thr = etai::get_model_thr();
        int    ma  = etai::get_model_ma_len();
        int    feat = 0;

        try { if (j.contains("best_thr")) thr = j["best_thr"].get<double>(); } catch (...) {}
        try { if (j.contains("ma_len"))   ma  = j["ma_len"].get<int>();      } catch (...) {}
        try { if (j.contains("policy") && j["policy"].contains("feat_dim"))
                  feat = j["policy"]["feat_dim"].get<int>(); } catch (...) {}

        etai::set_model_thr(thr);
        etai::set_model_ma_len(ma);
        if (feat > 0) etai::set_model_feat_dim(feat);
        etai::set_current_model(j);

        json r;
        r["ok"] = true;
        r["applied"] = { {"thr", thr}, {"ma_len", ma}, {"feat_dim", feat} };
        r["path"] = path;
        res.status = 200;
        res.set_content(r.dump(2), "application/json");
    });
}

} // namespace etai

// Глобальный форвардер
void register_model_routes(httplib::Server& svr) { etai::register_model_routes(svr); }
void register_model_route(httplib::Server& svr)  { etai::register_model_routes(svr); }

