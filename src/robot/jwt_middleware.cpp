#pragma once
#include "../json.hpp"
#include "../httplib.h"
#include <string>

using json = nlohmann::json;

namespace jwt_middleware {

// Простой base64 decode
static std::string base64_decode(const std::string& input) {
    static const std::string base64_chars = 
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    
    std::string output;
    int val = 0, valb = -8;
    
    for (unsigned char c : input) {
        if (c == '=') break;
        
        size_t pos = base64_chars.find(c);
        if (pos == std::string::npos) continue;
        
        val = (val << 6) + pos;
        valb += 6;
        
        if (valb >= 0) {
            output.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    
    return output;
}

int extract_user_id(const httplib::Request& req) {
    auto auth_header = req.get_header_value("Authorization");
    
    if (auth_header.empty()) {
        return -1;
    }
    
    if (auth_header.substr(0, 7) != "Bearer ") {
        return -1;
    }
    
    std::string token = auth_header.substr(7);
    
    size_t pos1 = token.find('.');
    size_t pos2 = token.find('.', pos1 + 1);
    
    if (pos1 == std::string::npos || pos2 == std::string::npos) {
        return -1;
    }
    
    std::string payload_b64 = token.substr(pos1 + 1, pos2 - pos1 - 1);
    
    // Decode payload
    std::string payload = base64_decode(payload_b64);
    
    try {
        json j = json::parse(payload);
        return j.value("user_id", -1);
    } catch(...) {
        return -1;
    }
}

bool require_auth(const httplib::Request& req, httplib::Response& res, int& user_id) {
    user_id = extract_user_id(req);
    
    if (user_id < 0) {
        json out = {
            {"ok", false},
            {"error", "unauthorized"},
            {"message", "Missing or invalid Authorization header"}
        };
        res.status = 401;
        res.set_content(out.dump(), "application/json");
        return false;
    }
    
    return true;
}

} // namespace jwt_middleware
