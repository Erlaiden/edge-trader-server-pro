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

}
