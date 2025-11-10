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

        pqxx::result check = txn.exec_params(
            "SELECT user_id FROM user_configs WHERE user_id = $1", user_id);

        if (check.empty()) {
            txn.exec_params(
                "INSERT INTO user_configs (user_id, symbol, leverage, balance_percent, "
                "tp_percent, sl_percent, min_confidence, auto_trade, check_interval) "
                "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)",
                user_id, symbol, leverage, balance_percent, tp_percent, sl_percent,
                min_confidence, auto_trade, check_interval);
        } else {
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

// ==================== TRADES FUNCTIONS ====================

int save_trade(int user_id, const std::string& symbol, const std::string& side,
               double qty, double entry_price, double tp_price, double sl_price) {
    std::lock_guard<std::mutex> lock(db_mutex);

    try {
        pqxx::connection conn(DB_CONN);
        pqxx::work txn(conn);

        pqxx::result res = txn.exec_params(
            "INSERT INTO trades (user_id, symbol, side, qty, entry_price, tp_price, sl_price, "
            "status, opened_at) VALUES ($1, $2, $3, $4, $5, $6, $7, 'open', NOW()) RETURNING id",
            user_id, symbol, side, qty, entry_price, tp_price, sl_price);

        int trade_id = res[0][0].as<int>();
        txn.commit();

        std::cout << "[DB] ✅ Saved trade id=" << trade_id << " " << symbol << " " << side
                  << " qty=" << qty << " entry=$" << entry_price << std::endl;
        return trade_id;

    } catch (const std::exception& e) {
        std::cerr << "[DB] ERROR save_trade: " << e.what() << std::endl;
        return -1;
    }
}

bool update_trade(int trade_id, double exit_price, double pnl, const std::string& status, const std::string& reason) {
    std::lock_guard<std::mutex> lock(db_mutex);

    try {
        pqxx::connection conn(DB_CONN);
        pqxx::work txn(conn);

        double pnl_percent = 0.0;
        
        // Получаем entry_price для расчета pnl_percent
        pqxx::result trade_res = txn.exec_params("SELECT entry_price, side FROM trades WHERE id = $1", trade_id);
        if (!trade_res.empty()) {
            double entry = trade_res[0][0].as<double>();
            std::string side = trade_res[0][1].as<std::string>();
            
            if (entry > 0 && exit_price > 0) {
                if (side == "Buy") {
                    pnl_percent = ((exit_price - entry) / entry) * 100.0;
                } else {
                    pnl_percent = ((entry - exit_price) / entry) * 100.0;
                }
            }
        }

        txn.exec_params(
            "UPDATE trades SET exit_price=$2, pnl=$3, pnl_percent=$4, status=$5, reason=$6, "
            "closed_at=NOW() WHERE id=$1",
            trade_id, exit_price, pnl, pnl_percent, status, reason);

        txn.commit();

        std::cout << "[DB] ✅ Updated trade id=" << trade_id << " exit=$" << exit_price
                  << " pnl=$" << pnl << " (" << pnl_percent << "%) status=" << status << std::endl;
        return true;

    } catch (const std::exception& e) {
        std::cerr << "[DB] ERROR update_trade: " << e.what() << std::endl;
        return false;
    }
}

bool mark_breakeven_activated(int trade_id) {
    std::lock_guard<std::mutex> lock(db_mutex);

    try {
        pqxx::connection conn(DB_CONN);
        pqxx::work txn(conn);

        txn.exec_params("UPDATE trades SET breakeven_activated=true WHERE id=$1", trade_id);
        txn.commit();

        std::cout << "[DB] ✅ Marked breakeven for trade id=" << trade_id << std::endl;
        return true;

    } catch (const std::exception& e) {
        std::cerr << "[DB] ERROR mark_breakeven_activated: " << e.what() << std::endl;
        return false;
    }
}

json get_user_trades(int user_id, int limit = 50) {
    std::lock_guard<std::mutex> lock(db_mutex);

    try {
        pqxx::connection conn(DB_CONN);
        pqxx::work txn(conn);

        pqxx::result res = txn.exec_params(
            "SELECT id, symbol, side, qty, entry_price, exit_price, tp_price, sl_price, "
            "pnl, pnl_percent, status, reason, breakeven_activated, opened_at, closed_at "
            "FROM trades WHERE user_id=$1 ORDER BY opened_at DESC LIMIT $2",
            user_id, limit);

        json trades = json::array();
        
        for (const auto& row : res) {
            json trade = {
                {"id", row[0].as<int>()},
                {"symbol", row[1].as<std::string>()},
                {"side", row[2].as<std::string>()},
                {"qty", row[3].as<double>()},
                {"entry_price", row[4].as<double>()},
                {"exit_price", row[5].is_null() ? 0.0 : row[5].as<double>()},
                {"tp_price", row[6].is_null() ? 0.0 : row[6].as<double>()},
                {"sl_price", row[7].is_null() ? 0.0 : row[7].as<double>()},
                {"pnl", row[8].is_null() ? 0.0 : row[8].as<double>()},
                {"pnl_percent", row[9].is_null() ? 0.0 : row[9].as<double>()},
                {"status", row[10].as<std::string>()},
                {"reason", row[11].is_null() ? "" : row[11].as<std::string>()},
                {"breakeven_activated", row[12].as<bool>()},
                {"opened_at", row[13].as<std::string>()},
                {"closed_at", row[14].is_null() ? "" : row[14].as<std::string>()}
            };
            trades.push_back(trade);
        }

        return trades;

    } catch (const std::exception& e) {
        std::cerr << "[DB] ERROR get_user_trades: " << e.what() << std::endl;
        return json::array();
    }
}

json get_user_pnl(int user_id) {
    std::lock_guard<std::mutex> lock(db_mutex);

    try {
        pqxx::connection conn(DB_CONN);
        pqxx::work txn(conn);

        // PnL сегодня (closed сделки)
        pqxx::result today_res = txn.exec_params(
            "SELECT COALESCE(SUM(pnl), 0) FROM trades "
            "WHERE user_id=$1 AND status!='open' AND DATE(closed_at)=CURRENT_DATE",
            user_id);

        double today_pnl = today_res[0][0].as<double>();

        // PnL всего (closed сделки)
        pqxx::result total_res = txn.exec_params(
            "SELECT COALESCE(SUM(pnl), 0) FROM trades "
            "WHERE user_id=$1 AND status!='open'",
            user_id);

        double total_pnl = total_res[0][0].as<double>();

        // Unrealized PnL (open позиции - требует текущей цены, пока 0)
        double unrealized_pnl = 0.0;

        return json{
            {"today", today_pnl},
            {"total", total_pnl},
            {"unrealized", unrealized_pnl}
        };

    } catch (const std::exception& e) {
        std::cerr << "[DB] ERROR get_user_pnl: " << e.what() << std::endl;
        return json{{"today", 0}, {"total", 0}, {"unrealized", 0}};
    }
}

int get_open_trade_id(int user_id, const std::string& symbol) {
    std::lock_guard<std::mutex> lock(db_mutex);

    try {
        pqxx::connection conn(DB_CONN);
        pqxx::work txn(conn);

        pqxx::result res = txn.exec_params(
            "SELECT id FROM trades WHERE user_id=$1 AND symbol=$2 AND status='open' "
            "ORDER BY opened_at DESC LIMIT 1",
            user_id, symbol);

        if (!res.empty()) {
            return res[0][0].as<int>();
        }

        return -1;

    } catch (const std::exception& e) {
        std::cerr << "[DB] ERROR get_open_trade_id: " << e.what() << std::endl;
        return -1;
    }
}

}
