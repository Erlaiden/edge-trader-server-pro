#pragma once
#include "../json.hpp"
#include <httplib.h>
#include <iostream>

using json = nlohmann::json;

namespace robot {

json etai_get_signal(const std::string& symbol) {
    try {
        // Делаем запрос к собственному API
        httplib::Client cli("localhost", 3000);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(5);
        
        std::string path = "/api/infer?symbol=" + symbol;
        auto res = cli.Get(path.c_str());
        
        if (!res || res->status != 200) {
            std::cout << "[SIGNAL] API request failed" << std::endl;
            return nullptr;
        }
        
        auto result = json::parse(res->body);
        
        if (!result.contains("signal") || !result.contains("confidence")) {
            std::cout << "[SIGNAL] Invalid signal format" << std::endl;
            return nullptr;
        }
        
        return result;
        
    } catch (std::exception& e) {
        std::cout << "[SIGNAL] Error: " << e.what() << std::endl;
        return nullptr;
    } catch (...) {
        std::cout << "[SIGNAL] Unknown error" << std::endl;
        return nullptr;
    }
}

} // namespace robot
