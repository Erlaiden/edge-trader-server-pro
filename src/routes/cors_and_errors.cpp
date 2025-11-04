// src/routes/cors_and_errors.cpp
#pragma once
#include <httplib.h>
#include "json.hpp"
using json = nlohmann::json;

inline void enable_cors_and_errors(httplib::Server& svr){
    // CORS headers
    svr.set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Methods", "GET,POST,OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type, Authorization"}
    });
    // Preflight
    svr.Options(R"(.*)", [](const httplib::Request&, httplib::Response& res){
        res.status = 204;
        res.set_header("Access-Control-Max-Age","86400");
    });
    // 404/405 -> JSON
    svr.set_error_handler([](const httplib::Request& req, httplib::Response& res){
        if(res.status==405){
            json j = {{"ok", false}, {"error", "method_not_allowed"}, {"path", req.path}};
            res.set_content(j.dump(2), "application/json");
            return;
        }
        json j = {{"ok", false}, {"error", "not_found"}, {"path", req.path}};
        res.status = 404;
        res.set_content(j.dump(2), "application/json");
    });
}
