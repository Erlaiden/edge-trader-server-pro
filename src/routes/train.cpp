#include "routes/train.h"
#include "train_logic.h"
#include "json.hpp"
#include <httplib.h>
#include <string>

using json = nlohmann::json;
using namespace httplib;

static inline std::string qs(const Request& req, const char* k, const char* defv){
    return req.has_param(k) ? req.get_param_value(k) : std::string(defv);
}

// Поднимаем ключевые метрики из .metrics на верхний уровень (дублируем, не ломаем контракт)
static void promote_metrics(json& j){
    if(!j.contains("metrics") || !j["metrics"].is_object()) return;
    const json& m = j["metrics"];
    auto copy = [&](const char* k){
        if(m.contains(k)) j[k] = m.at(k);
    };
    // основные
    copy("val_accuracy");
    copy("val_reward");
    copy("M_labeled");
    copy("val_size");
    // расширенные
    copy("N_rows");
    copy("raw_cols");
    copy("feat_cols");
}

void register_train_routes(Server& svr){
    svr.Get("/api/train", [](const Request& req, Response& res){
        try{
            const std::string symbol   = qs(req, "symbol",   "BTCUSDT");
            const std::string interval = qs(req, "interval", "15");

            int    episodes = 40;
            double tp = 0.008, sl = 0.0032;
            int    ma = 12;

            try { episodes = std::stoi(qs(req,"episodes","40")); } catch(...) {}
            try { tp       = std::stod(qs(req,"tp","0.008"));   } catch(...) {}
            try { sl       = std::stod(qs(req,"sl","0.0032"));  } catch(...) {}
            try { ma       = std::stoi(qs(req,"ma","12"));      } catch(...) {}

            json out = etai::run_train_pro_and_save(symbol, interval, episodes, tp, sl, ma);

            // Всегда дублируем ключевые метрики на верхний уровень
            promote_metrics(out);

            res.set_content(out.dump(2), "application/json");
        }catch(const std::exception& e){
            json err = {{"ok",false},{"error","train_handler_exception"},{"error_detail",e.what()}};
            res.set_content(err.dump(2), "application/json");
        }catch(...){
            json err = {{"ok",false},{"error","train_handler_unknown"},{"error_detail","unknown"}};
            res.set_content(err.dump(2), "application/json");
        }
    });
}
