#include "json.hpp"
#include <httplib.h>
#include <random>
#include <map>
#include <mutex>
#include <chrono>
#include <ctime>
#include "../robot/smtp.cpp"

using json = nlohmann::json;

// Временное хранилище кодов (email -> {code, expires_at})
static std::map<std::string, std::pair<std::string, long long>> verification_codes;
static std::mutex codes_mutex;

// Генерация 6-значного кода
static std::string generate_code() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(100000, 999999);
    return std::to_string(dis(gen));
}

// Отправка email (пока логируем, потом добавим SMTP)
static bool send_email(const std::string& to, const std::string& code) {
    if (!smtp::send_verification_email(to, code)) {
        return false;
    }
    return true;
}

void register_auth_routes(httplib::Server& srv) {
    
    srv.Post("/api/auth/send-code", [&](const httplib::Request& req, httplib::Response& res){
        json out{{"ok", false}};
        
        try {
            json in = json::parse(req.body);
            std::string email = in.value("email", "");
            
            if (email.empty() || email.find('@') == std::string::npos) {
                out["error"] = "invalid_email";
                res.set_content(out.dump(), "application/json");
                return;
            }
            
            std::string code = generate_code();
            long long expires_at = std::time(nullptr) + 600; // 10 минут
            
            {
                std::lock_guard<std::mutex> lock(codes_mutex);
                verification_codes[email] = {code, expires_at};
            }
            
            if (!send_email(email, code)) {
                out["error"] = "email_send_failed";
                res.set_content(out.dump(), "application/json");
                return;
            }
            
            out["ok"] = true;
            out["message"] = "code_sent";
            out["expires_in"] = 600;
            res.set_content(out.dump(), "application/json");
            
        } catch(...) {
            out["error"] = "exception";
            res.set_content(out.dump(), "application/json");
        }
    });
    
    srv.Post("/api/auth/verify-code", [&](const httplib::Request& req, httplib::Response& res){
        json out{{"ok", false}};
        
        try {
            json in = json::parse(req.body);
            std::string email = in.value("email", "");
            std::string code = in.value("code", "");
            
            if (email.empty() || code.empty()) {
                out["error"] = "missing_params";
                res.set_content(out.dump(), "application/json");
                return;
            }
            
            {
                std::lock_guard<std::mutex> lock(codes_mutex);
                auto it = verification_codes.find(email);
                
                if (it == verification_codes.end()) {
                    out["error"] = "code_not_found";
                    res.set_content(out.dump(), "application/json");
                    return;
                }
                
                auto [stored_code, expires_at] = it->second;
                long long now = std::time(nullptr);
                
                if (now > expires_at) {
                    verification_codes.erase(it);
                    out["error"] = "code_expired";
                    res.set_content(out.dump(), "application/json");
                    return;
                }
                
                if (stored_code != code) {
                    out["error"] = "code_mismatch";
                    res.set_content(out.dump(), "application/json");
                    return;
                }
                
                verification_codes.erase(it);
            }
            
            out["ok"] = true;
            out["verified"] = true;
            res.set_content(out.dump(), "application/json");
            
        } catch(...) {
            out["error"] = "exception";
            res.set_content(out.dump(), "application/json");
        }
    });
}
