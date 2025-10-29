// Extended AI health: adds train_telemetry and policy_stats.
//
// This file provides register_health_ai(httplib::Server&) to match health.cpp call.

#include "json.hpp"
#include "httplib.h"
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

using json = nlohmann::json;

// Forward declarations from the server core.
// These should already exist elsewhere in the codebase.
namespace etai {
    // Returns current in-memory model JSON (or null json if not loaded).
    json get_current_model();
    // Exported scalar settings
    double get_model_thr();
    int    get_model_ma_len();
    // Optional: data health summary; if not linked we guard by try/catch.
    json   get_data_health();
}

// ---- helpers ----
static inline json safe_read_json_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.good()) return json(); // null
    try {
        json j; f >> j;
        return j;
    } catch (...) {
        return json(); // null
    }
}

static inline json build_policy_stats(const json& model) {
    json out = json::object();
    if (!model.is_object() || !model.contains("policy")) return out;

    const json& P = model["policy"];
    out["source"]   = model.value("policy_source", std::string()); // "learn" | "heuristic" | ""
    out["feat_dim"] = P.value("feat_dim", 0);

    if (P.contains("W") && P["W"].is_array()) {
        const auto& W = P["W"];
        out["W_len"] = (unsigned)W.size();

        // first 8 weights
        json head = json::array();
        for (size_t i = 0; i < std::min<size_t>(8, W.size()); ++i) head.push_back(W[i]);
        out["W_head"] = head;

        // uniq/min/max/mean
        std::vector<double> wv; wv.reserve(W.size());
        for (const auto& v : W) {
            if (v.is_number()) wv.push_back(v.get<double>());
            else if (v.is_string()) {
                try { wv.push_back(std::stod(v.get<std::string>())); } catch (...) {}
            }
        }
        std::sort(wv.begin(), wv.end());
        int uniq = 0;
        for (size_t i=0;i<wv.size();) {
            size_t j=i+1;
            while (j<wv.size() && std::fabs(wv[j]-wv[i])<1e-15) ++j;
            ++uniq; i=j;
        }
        out["uniq_W"] = uniq;
        if (!wv.empty()) {
            double mn = wv.front(), mx = wv.back(), s = 0.0;
            for (double x: wv) s += x;
            out["W_min"]  = mn;
            out["W_max"]  = mx;
            out["W_mean"] = s / (double)wv.size();
        }
    }
    if (P.contains("b") && P["b"].is_array()) {
        out["b_len"] = (unsigned)P["b"].size();
    }
    return out;
}

// ---- public entry point called from health.cpp ----
void register_health_ai(httplib::Server& srv) {
    // Base: /api/health/ai
    srv.Get(R"(/api/health/ai)",
        [](const httplib::Request& req, httplib::Response& res){
            json out;
            out["ok"] = true;

            // model basics
            json model = etai::get_current_model();
            out["model"]        = model.is_null() ? json() : model;
            out["model_thr"]    = etai::get_model_thr();
            out["model_ma_len"] = etai::get_model_ma_len();

            // optional data health
            try {
                json d = etai::get_data_health();
                if (!d.is_null()) out["data"] = d;
            } catch (...) {}

            // training telemetry (if exists)
            json telem = safe_read_json_file("cache/logs/last_train_telemetry.json");
            if (!telem.is_null()) out["train_telemetry"] = telem;

            // compact policy stats
            out["policy_stats"] = build_policy_stats(model);

            res.set_content(out.dump(2), "application/json");
        }
    );

    // Explicit extended view
    srv.Get(R"(/api/health/ai/extended)",
        [](const httplib::Request& req, httplib::Response& res){
            json out;
            out["ok"] = true;

            json model = etai::get_current_model();
            out["model"]        = model.is_null() ? json() : model;
            out["model_thr"]    = etai::get_model_thr();
            out["model_ma_len"] = etai::get_model_ma_len();

            try {
                json d = etai::get_data_health();
                if (!d.is_null()) out["data"] = d;
            } catch (...) {}

            json telem = safe_read_json_file("cache/logs/last_train_telemetry.json");
            out["train_telemetry"] = telem.is_null() ? json() : telem;
            out["policy_stats"]    = build_policy_stats(model);

            res.set_content(out.dump(2), "application/json");
        }
    );
}
