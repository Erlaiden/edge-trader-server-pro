#pragma once
#include "json.hpp"
#include <string>

using nlohmann::json;

// Унифицированный формат HTTP-ответа
inline json make_reply(bool ok,
                       const std::string& msg,
                       const json& data = json::object()) {
    json r;
    r["ok"] = ok;
    if (!msg.empty()) r["msg"] = msg;
    if (!data.is_null()) r["data"] = data;
    return r;
}

// Обёртка для train-результата (с явным model_path)
inline json make_train_reply(const json& trainer_json,
                             double tp, double sl, int ma_len,
                             const std::string& model_path) {
    json r;
    r["ok"]          = trainer_json.value("ok", true);
    r["tp"]          = tp;
    r["sl"]          = sl;
    r["ma_len"]      = ma_len;
    r["best_thr"]    = trainer_json.value("best_thr", 0.0);
    r["metrics"]     = trainer_json.value("metrics", json::object());
    r["model_path"]  = model_path;
    r["schema"]      = trainer_json.value("schema", "");
    r["mode"]        = trainer_json.value("mode", "");
    r["policy_source"]= trainer_json.value("policy_source", "");
    return r;
}
