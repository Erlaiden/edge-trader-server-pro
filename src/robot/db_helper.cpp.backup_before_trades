#pragma once
#include "../json.hpp"
#include <pqxx/pqxx>
#include <string>
#include <iostream>
#include <mutex>

using json = nlohmann::json;

namespace db {

static std::mutex db_mutex;
static const char* DB_CONN = "host=localhost port=5432 dbname=edge_trader_prod user=edge_user password=edge_secure_2025";

int get_or_create_user(const std::string& email) {
    std::lock_guard<std::mutex> lock(db_mutex);
    
    try {
        pqxx::connection conn(DB_CONN);
        pqxx::work txn(conn);
        
        pqxx::result res = txn.exec_params("SELECT id FROM users WHERE email = $1", email);
        
        if (!res.empty()) {
            int user_id = res[0][0].as<int>();
            txn.exec_params("UPDATE users SET last_login_at = NOW() WHERE id = $1", user_id);
            txn.commit();
            return user_id;
        }
        
        pqxx::result insert_res = txn.exec_params(
            "INSERT INTO users (email, subscription_tier, created_at, last_login_at) "
            "VALUES ($1, 'free', NOW(), NOW()) RETURNING id", email);
        
        int user_id = insert_res[0][0].as<int>();
        
        txn.exec_params(
            "INSERT INTO user_configs (user_id, symbol, leverage, balance_percent, "
            "tp_percent, sl_percent, min_confidence, auto_trade, check_interval) "
            "VALUES ($1, 'BTCUSDT', 10, 90.0, 2.0, 1.0, 60.0, false, 60)", user_id);
        
        txn.commit();
        
        std::cout << "[DB] Created new user: " << email << " (id=" << user_id << ")" << std::endl;
        return user_id;
        
    } catch (const std::exception& e) {
        std::cerr << "[DB] ERROR get_or_create_user: " << e.what() << std::endl;
        return -1;
    }
}

bool save_user_api_keys(int user_id, const std::string& api_key, const std::string& api_secret, bool testnet) {
    std::lock_guard<std::mutex> lock(db_mutex);
    
    try {
        pqxx::connection conn(DB_CONN);
        pqxx::work txn(conn);
        
        std::cerr << "[DB] DEBUG: Saving keys for user_id=" << user_id << std::endl;
        
        txn.exec_params("DELETE FROM user_api_keys WHERE user_id = $1", user_id);
        
        txn.exec_params(
            "INSERT INTO user_api_keys (user_id, api_key_encrypted, api_secret_encrypted, testnet) "
            "VALUES ($1, $2, $3, $4)", user_id, api_key, api_secret, testnet);
        
        txn.commit();
        
        std::cout << "[DB] ✅ Saved API keys for user_id=" << user_id << std::endl;
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "[DB] ERROR save_user_api_keys: " << e.what() << std::endl;
        return false;
    }
}

json get_user_api_keys(int user_id) {
    std::lock_guard<std::mutex> lock(db_mutex);
    
    try {
        pqxx::connection conn(DB_CONN);
        pqxx::work txn(conn);
        
        pqxx::result res = txn.exec_params(
            "SELECT api_key_encrypted, api_secret_encrypted, testnet FROM user_api_keys WHERE user_id = $1",
            user_id);
        
        if (res.empty()) {
            return json::object();
        }
        
        return json{
            {"apiKey", res[0][0].as<std::string>()},
            {"apiSecret", res[0][1].as<std::string>()},
            {"testnet", res[0][2].as<bool>()}
        };
        
    } catch (const std::exception& e) {
        std::cerr << "[DB] ERROR get_user_api_keys: " << e.what() << std::endl;
        return json::object();
    }
}


json get_user_config(int user_id) {
    std::lock_guard<std::mutex> lock(db_mutex);

    try {
        pqxx::connection conn(DB_CONN);
        pqxx::work txn(conn);

        pqxx::result res = txn.exec_params(
            "SELECT symbol, leverage, balance_percent, tp_percent, sl_percent, "
            "min_confidence, auto_trade, check_interval FROM user_configs WHERE user_id = $1",
            user_id);

        if (res.empty()) {
            // Возвращаем дефолтный конфиг если не найден
            return json{
                {"symbol", "BTCUSDT"},
                {"leverage", 10},
                {"balancePercent", 90.0},
                {"tpPercent", 2.0},
                {"slPercent", 1.0},
                {"minConfidence", 60.0},
                {"autoTrade", false},
                {"checkInterval", 60}
            };
        }

        return json{
            {"symbol", res[0][0].as<std::string>()},
            {"leverage", res[0][1].as<int>()},
            {"balancePercent", res[0][2].as<double>()},
            {"tpPercent", res[0][3].as<double>()},
            {"slPercent", res[0][4].as<double>()},
            {"minConfidence", res[0][5].as<double>()},
            {"autoTrade", res[0][6].as<bool>()},
            {"checkInterval", res[0][7].as<int>()}
        };

    } catch (const std::exception& e) {
        std::cerr << "[DB] ERROR get_user_config: " << e.what() << std::endl;
        return json{
            {"symbol", "BTCUSDT"},
            {"leverage", 10},
            {"balancePercent", 90.0},
            {"tpPercent", 2.0},
            {"slPercent", 1.0},
            {"minConfidence", 60.0},
            {"autoTrade", false},
            {"checkInterval", 60}
        };
    }
}


bool save_user_config(int user_id, const json& config) {
    std::lock_guard<std::mutex> lock(db_mutex);

    try {
        pqxx::connection conn(DB_CONN);
        pqxx::work txn(conn);

        std::string symbol = config.value("symbol", "BTCUSDT");
        int leverage = config.value("leverage", 10);
        double balance_percent = config.value("balancePercent", 90.0);
        double tp_percent = config.value("tpPercent", 2.0);
        double sl_percent = config.value("slPercent", 1.0);
        double min_confidence = config.value("minConfidence", 60.0);
        bool auto_trade = config.value("autoTrade", false);
        int check_interval = config.value("checkInterval", 60);

        // UPDATE или INSERT если не существует
        pqxx::result check = txn.exec_params(
            "SELECT user_id FROM user_configs WHERE user_id = $1", user_id);

        if (check.empty()) {
            // INSERT
            txn.exec_params(
                "INSERT INTO user_configs (user_id, symbol, leverage, balance_percent, "
                "tp_percent, sl_percent, min_confidence, auto_trade, check_interval) "
                "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)",
                user_id, symbol, leverage, balance_percent, tp_percent, sl_percent,
                min_confidence, auto_trade, check_interval);
        } else {
            // UPDATE
            txn.exec_params(
                "UPDATE user_configs SET symbol=$2, leverage=$3, balance_percent=$4, "
                "tp_percent=$5, sl_percent=$6, min_confidence=$7, auto_trade=$8, "
                "check_interval=$9 WHERE user_id=$1",
                user_id, symbol, leverage, balance_percent, tp_percent, sl_percent,
                min_confidence, auto_trade, check_interval);
        }

        txn.commit();

        std::cout << "[DB] ✅ Saved config for user_id=" << user_id 
                  << " symbol=" << symbol << " leverage=" << leverage << std::endl;
        return true;

    } catch (const std::exception& e) {
        std::cerr << "[DB] ERROR save_user_config: " << e.what() << std::endl;
        return false;
    }
}

}
