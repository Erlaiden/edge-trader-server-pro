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

static inline bool qs_bool(const Request& req, const char* k, bool defv){
    if(!req.has_param(k)) return defv;
    auto v = req.get_param_value(k);
    return (v=="1"||v=="true"||v=="yes"||v=="on");
}

static json flatten_metrics(json j){
    try{
        if(j.contains("metrics") && j["metrics"].is_object()){
            const auto& m = j["metrics"];
            auto put = [&](const char* key){
                if(m.contains(key)) j[key] = m.at(key);
            };
            put("val_accuracy");
            put("val_reward");
            put("M_labeled");
            put("val_size");
        }
    }catch(...){}
    return j;
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

            // Если flat=1 — развернём метрики на верхний уровень (для стабильного клиента)
            if (qs_bool(req, "flat", false)) {
                out = flatten_metrics(out);
            }

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
