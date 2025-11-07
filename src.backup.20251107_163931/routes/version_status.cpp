// src/routes/version_status.cpp
#pragma once
#include <httplib.h>
#include <string>
#include <chrono>
#include <ctime>
#include "json.hpp"
#include "../server_accessors.h"

using json = nlohmann::json;

namespace {
inline const char* getenv_def(const char* k, const char* d){
    if(const char* v = std::getenv(k)) return (*v? v : d);
    return d;
}
inline std::string now_iso8601(){
    std::time_t t = std::time(nullptr);
    char buf[64]; std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}
}
static std::chrono::steady_clock::time_point& boot_ts() {
    static auto ts = std::chrono::steady_clock::now();
    return ts;
}

inline void register_version_status_routes(httplib::Server& svr) {
    // /api/version
    svr.Get("/api/version", [](const httplib::Request&, httplib::Response& res){
        json j;
        j["ok"]        = true;
        j["app"]       = "EdgeTrader";
        j["api"]       = 1;
        j["git"]       = getenv_def("ETAI_GIT_SHA", "");
        j["build_tag"] = getenv_def("ETAI_BUILD_TAG", "");
        j["build_time"]= getenv_def("ETAI_BUILD_TIME", "");
        j["now"]       = now_iso8601();
        try {
            json m = etai::get_current_model();
            json ms = {
                {"best_thr", m.value("best_thr", json(nullptr))},
                {"ma_len",   m.value("ma_len",   json(nullptr))},
                {"tp",       m.value("tp",       json(nullptr))},
                {"sl",       m.value("sl",       json(nullptr))},
                {"feat_dim", m.contains("policy") && m["policy"].contains("feat_dim")
                                ? m["policy"]["feat_dim"] : json(nullptr)},
                {"version",  m.value("version",  json(nullptr))},
                {"schema",   m.value("schema",   json(nullptr))},
                {"mode",     m.value("mode",     json(nullptr))},
                {"symbol",   m.value("symbol",   json(nullptr))},
                {"interval", m.value("interval", json(nullptr))},
                {"model_path", m.value("model_path", json(nullptr))}
            };
            j["model"] = ms;
        } catch(...) { j["model"] = json::object(); }
        res.status = 200;
        res.set_content(j.dump(2), "application/json");
    });

    // /api/status
    svr.Get("/api/status", [](const httplib::Request&, httplib::Response& res){
        using namespace std::chrono;
        auto up_ms = duration_cast<milliseconds>(steady_clock::now() - boot_ts()).count();
        json st;
        st["ok"] = true;
        st["uptime_ms"] = up_ms;
        st["pid"] = static_cast<int>(::getpid());
        st["thr"]   = etai::get_model_thr();
        st["ma"]    = etai::get_model_ma_len();
        st["feat"]  = etai::get_model_feat_dim();
        st["flags"] = {
            {"ETAI_AGENT_ENABLE",       getenv_def("ETAI_AGENT_ENABLE","")},
            {"ETAI_ENABLE_TRAIN_ENV",   getenv_def("ETAI_ENABLE_TRAIN_ENV","")},
            {"ETAI_MTF_ENABLE",         getenv_def("ETAI_MTF_ENABLE","")},
            {"ETAI_ENABLE_ANTI_MANIP",  getenv_def("ETAI_ENABLE_ANTI_MANIP","")}
        };
        res.status = 200;
        res.set_content(st.dump(2), "application/json");
    });
}
