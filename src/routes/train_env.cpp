#pragma once
#include <httplib.h>
#include "json.hpp"
#include <cstdlib>

static inline bool feature_on(const char* k) {
    const char* s = std::getenv(k);
    if (!s || !*s) return false;
    return (s[0]=='1') || (s[0]=='T') || (s[0]=='t') || (s[0]=='Y') || (s[0]=='y');
}

static inline void register_train_env_routes(httplib::Server& svr) {
    svr.Get("/api/train_env", [](const httplib::Request& req, httplib::Response& res) {
        using json = nlohmann::json;
        if (!feature_on("ETAI_ENABLE_TRAIN_ENV")) {
            json j = {
                {"ok", false},
                {"error", "feature_disabled"},
                {"hint", "export ETAI_ENABLE_TRAIN_ENV=1 (service env)"},
                {"version", "env_v1_rfc"}
            };
            res.set_content(j.dump(2), "application/json");
            return;
        }

        // Пока заглушка. Реализация появится на этапе B.
        json j = {
            {"ok", false},
            {"error", "not_implemented"},
            {"version", "env_v1_rfc"}
        };
        res.set_content(j.dump(2), "application/json");
    });
}
