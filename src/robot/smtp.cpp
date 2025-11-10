#pragma once
#include "../json.hpp"
#include <curl/curl.h>
#include <fstream>
#include <string>
#include <sstream>

using json = nlohmann::json;

namespace smtp {

struct Config {
    std::string host;
    int port;
    std::string username;
    std::string password;
    std::string from;
    std::string from_name;
};

static Config load_config() {
    Config cfg;
    std::ifstream f("/var/lib/edge-trader/smtp/config.json");
    if (!f) return cfg;
    
    try {
        json j;
        f >> j;
        cfg.host = j.value("host", "");
        cfg.port = j.value("port", 587);
        cfg.username = j.value("username", "");
        cfg.password = j.value("password", "");
        cfg.from = j.value("from", "");
        cfg.from_name = j.value("from_name", "Edge Trader AI");
    } catch(...) {}
    
    return cfg;
}

static size_t payload_source(char* ptr, size_t size, size_t nmemb, void* userp) {
    std::string* data = static_cast<std::string*>(userp);
    size_t len = size * nmemb;
    
    if (data->empty()) return 0;
    
    size_t to_copy = std::min(len, data->size());
    memcpy(ptr, data->c_str(), to_copy);
    data->erase(0, to_copy);
    
    return to_copy;
}

bool send_verification_email(const std::string& to, const std::string& code) {
    Config cfg = load_config();
    
    if (cfg.host.empty() || cfg.username.empty()) {
        std::cerr << "[SMTP] Config not loaded" << std::endl;
        return false;
    }
    
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    
    // Email payload
    std::ostringstream payload;
    payload << "From: " << cfg.from_name << " <" << cfg.from << ">\r\n";
    payload << "To: <" << to << ">\r\n";
    payload << "Subject: Edge Trader AI - Verification Code\r\n";
    payload << "Content-Type: text/html; charset=UTF-8\r\n";
    payload << "\r\n";
    payload << "<html><body style='font-family: Arial, sans-serif;'>\r\n";
    payload << "<h2>Edge Trader AI - Email Verification</h2>\r\n";
    payload << "<p>Your verification code is:</p>\r\n";
    payload << "<h1 style='color: #4CAF50; font-size: 48px;'>" << code << "</h1>\r\n";
    payload << "<p>This code will expire in 10 minutes.</p>\r\n";
    payload << "<p>If you didn't request this code, please ignore this email.</p>\r\n";
    payload << "<hr>\r\n";
    payload << "<p style='color: #888;'>Edge Trader AI - Automated Trading Platform</p>\r\n";
    payload << "</body></html>\r\n";
    
    std::string payload_str = payload.str();
    
    // SMTP settings
    std::string url = "smtp://" + cfg.host + ":" + std::to_string(cfg.port);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL);
    curl_easy_setopt(curl, CURLOPT_USERNAME, cfg.username.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, cfg.password.c_str());
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM, cfg.from.c_str());
    
    struct curl_slist* recipients = NULL;
    recipients = curl_slist_append(recipients, to.c_str());
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);
    
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, payload_source);
    curl_easy_setopt(curl, CURLOPT_READDATA, &payload_str);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);
    
    CURLcode res = curl_easy_perform(curl);
    
    curl_slist_free_all(recipients);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK) {
        std::cerr << "[SMTP] Failed: " << curl_easy_strerror(res) << std::endl;
        return false;
    }
    
    std::cout << "[SMTP] ✅ Email sent to " << to << std::endl;
    return true;
}

} // namespace smtp
