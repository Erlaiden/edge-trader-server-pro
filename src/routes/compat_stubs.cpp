// src/routes/compat_stubs.cpp
#pragma once
#include <httplib.h>
#include "json.hpp"
using json = nlohmann::json;

inline void register_compat_stubs(httplib::Server& svr){
    auto not_impl = [](const char* path){
        return [path](const httplib::Request& req, httplib::Response& res){
            json j; j["ok"]=false; j["error"]="not_implemented"; j["path"]=path;
            if(!req.params.empty()){
                json q=json::object();
                for(auto& kv:req.params) q[kv.first]=kv.second;
                j["query"]=q;
            }
            res.status=501;
            res.set_content(j.dump(2),"application/json");
        };
    };

    svr.Get("/api/model/read",                not_impl("/api/model/read"));
    svr.Get("/api/infer/batch",               not_impl("/api/infer/batch"));
    svr.Get("/api/pipeline/prepare_train",    not_impl("/api/pipeline/prepare_train"));
    svr.Get("/api/agents/test",               not_impl("/api/agents/test"));
    svr.Get("/api/robot/keys",                not_impl("/api/robot/keys"));
    svr.Get("/api/robot/start",               not_impl("/api/robot/start"));
    svr.Get("/api/robot/stop",                not_impl("/api/robot/stop"));
    svr.Get("/api/model/status",              not_impl("/api/model/status"));
}
