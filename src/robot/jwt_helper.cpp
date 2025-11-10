#pragma once
#include "../json.hpp"
#include "../httplib.h"
#include <string>
#include <chrono>

using json = nlohmann::json;

namespace jwt_helper {

static const std::string JWT_SECRET = "edge_trader_secret_key_2025_change_me_in_production";

std::string generate_token(int user_id, const std::string& email, const std::string& tier = "free") {
    auto now = std::chrono::system_clock::now();
    auto exp = now + std::chrono::hours(24 * 30);
    
    long long iat = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    long long exp_time = std::chrono::duration_cast<std::chrono::seconds>(exp.time_since_epoch()).count();
    
    json payload = {
        {"user_id", user_id},
        {"email", email},
        {"tier", tier},
        {"iat", iat},
        {"exp", exp_time}
    };
    
    // Используем base64 из httplib
    std::string token = "eyJ0eXAiOiJKV1QiLCJhbGciOiJIUzI1NiJ9." + 
                        httplib::detail::base64_encode(payload.dump()) + 
                        ".sig_placeholder";
    
    return token;
}

bool verify_token(const std::string& token, int& user_id, std::string& email) {
    // TODO: Полная проверка с jwt-cpp
    return true;
}

} // namespace jwt_helper
