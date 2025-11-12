#pragma once
#include "json.hpp"
#include <string>

// Обогащаем ответ текущими эффективными коэффициентами
#include "rewardv2_accessors.h"

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
    r["ok"]            = trainer_json.value("ok", true);
    r["tp"]            = tp;
    r["sl"]            = sl;
    r["ma_len"]        = ma_len;
    r["best_thr"]      = trainer_json.value("best_thr", 0.0);
    r["schema"]        = trainer_json.value("schema", "");
    r["mode"]          = trainer_json.value("mode", "");
    r["policy_source"] = trainer_json.value("policy_source", "");
    r["version"]       = trainer_json.value("version", 0);
    r["model_path"]    = model_path;

    // Копия метрик тренера
    json m = trainer_json.value("metrics", json::object());

    // Если тренер не положил эффективные коэффициенты — дольём из атомиков (как в /metrics)
    if (!m.contains("val_lambda_eff") || m["val_lambda_eff"].is_null()) {
        m["val_lambda_eff"] = etai::get_lambda_risk_eff();
    }
    if (!m.contains("val_mu_eff") || m["val_mu_eff"].is_null()) {
        m["val_mu_eff"] = etai::get_mu_manip_eff();
    }
    r["metrics"] = m;

    // Протащим policy целиком (включая norm), если пришёл
    if (trainer_json.contains("policy") && trainer_json["policy"].is_object()) {
        const json& P = trainer_json["policy"];
        r["policy"] = P;

        // Удобные флаги и длины на верхнем уровне
        bool has_norm = P.contains("norm") && P["norm"].is_object()
                      && P["norm"].contains("mu") && P["norm"].contains("sd")
                      && P["norm"]["mu"].is_array() && P["norm"]["sd"].is_array();
        r["has_norm"] = has_norm;
        if (has_norm) {
            r["mu_len"] = (int)P["norm"]["mu"].size();
            r["sd_len"] = (int)P["norm"]["sd"].size();
        } else {
            r["mu_len"] = 0;
            r["sd_len"] = 0;
        }
    } else {
        r["has_norm"] = false;
        r["mu_len"]   = 0;
        r["sd_len"]   = 0;
    }

    // Copy PPO_RL weights if present
    if (trainer_json.contains("actor_weights")) {
        r["actor_weights"] = trainer_json["actor_weights"];
    }
    if (trainer_json.contains("critic_weights")) {
        r["critic_weights"] = trainer_json["critic_weights"];
    }
    return r;
}
