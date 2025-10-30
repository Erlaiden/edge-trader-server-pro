#include "routes/train.h"
#include "train_logic.h"
#include "server_accessors.h"
#include "json.hpp"
#include <httplib.h>
#include <string>

using json = nlohmann::json;
using namespace httplib;

static inline std::string get_qs(const Request& req, const char* k, const char* defv) {
    return req.has_param(k) ? req.get_param_value(k) : std::string(defv);
}

void register_train_routes(Server& svr) {
    svr.Get("/api/train", [](const Request& req, Response& res) {
        try {
            const std::string symbol   = get_qs(req, "symbol",   "BTCUSDT");
            const std::string interval = get_qs(req, "interval", "15");

            int episodes = 40;
            double tp = 0.008;
            double sl = 0.0032;
            int ma = 12;

            try { episodes = std::stoi(get_qs(req, "episodes", "40")); } catch (...) {}
            try { tp       = std::stod(get_qs(req, "tp",       "0.008")); } catch (...) {}
            try { sl       = std::stod(get_qs(req, "sl",       "0.0032")); } catch (...) {}
            try { ma       = std::stoi(get_qs(req, "ma",       "12")); } catch (...) {}

            json result = etai::run_train_pro_and_save(symbol, interval, episodes, tp, sl, ma);
            res.set_content(result.dump(2), "application/json");
        } catch (const std::exception& e) {
            json err = {{"ok", false}, {"error", "train_handler_exception"}, {"error_detail", e.what()}};
            res.set_content(err.dump(2), "application/json");
        } catch (...) {
            json err = {{"ok", false}, {"error", "train_handler_unknown"}, {"error_detail", "unknown error"}};
            res.set_content(err.dump(2), "application/json");
        }
    });
}
