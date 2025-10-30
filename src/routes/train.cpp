#include "json.hpp"
#include "httplib.h"
#include "http_helpers.h"   // qp()
#include <string>
#include <stdexcept>
#include <sstream>

using json = nlohmann::json;

// Форвард: реализация в train_logic.cpp (оставляем существующую сигнатуру)
json run_train_pro_and_save(const std::string& symbol,
                            const std::string& interval,
                            int episodes,
                            double tp,
                            double sl,
                            int ma_len);

static inline int as_int(const std::string& s, int def){
    if(s.empty()) return def;
    try { return std::stoi(s); } catch (...) { return def; }
}
static inline double as_double(const std::string& s, double def){
    if(s.empty()) return def;
    try { return std::stod(s); } catch (...) { return def; }
}

void register_train_routes(httplib::Server& srv){
    // Основной роут обучения PRO
    srv.Get("/api/train", [&](const httplib::Request& req, httplib::Response& res){
        json out = json::object();
        try {
            const std::string symbol   = qp(req, "symbol",   "BTCUSDT");
            const std::string interval = qp(req, "interval", "15");

            int episodes = as_int(qp(req,"episodes","40"), 40);
            double tp    = as_double(qp(req,"tp","0.008"),   0.008);
            double sl    = as_double(qp(req,"sl","0.0032"),  0.0032);
            int ma_len   = as_int(qp(req,"ma","12"), 12);

            // Валидации базовые (без фанатизма, чтобы не падать на мусоре)
            if(episodes <= 0 || episodes > 2000) throw std::runtime_error("bad_param:episodes");
            if(!(tp>0 && tp<0.05))               throw std::runtime_error("bad_param:tp");
            if(!(sl>0 && sl<0.05))               throw std::runtime_error("bad_param:sl");
            if(!(ma_len>=1 && ma_len<=200))      throw std::runtime_error("bad_param:ma");

            json m = run_train_pro_and_save(symbol, interval, episodes, tp, sl, ma_len);

            // Нормализуем ответ: всегда есть .ok; метрики под .metrics
            out["ok"] = m.value("ok", false);
            out["model_path"] = m.value("model_path", m.value("path", "")); // совместимость
            if(m.contains("metrics") && m["metrics"].is_object()){
                out["metrics"] = m["metrics"];
            } else {
                // Соберём ключевые метрики, если они в корне (совместимость со старыми версиями)
                json mx;
                if(m.contains("best_thr"))     mx["best_thr"] = m["best_thr"];
                if(m.contains("totalReward"))  mx["totalReward"] = m["totalReward"];
                if(m.contains("val_accuracy")) mx["val_accuracy"] = m["val_accuracy"];
                if(!mx.empty()) out["metrics"] = mx;
            }
            // Прокинем служебное
            if(m.contains("schema")) out["schema"] = m["schema"];
            if(m.contains("mode"))   out["mode"]   = m["mode"];

            res.set_content(out.dump(2), "application/json");
            return;
        }
        catch(const std::exception& e){
            out["ok"] = false;
            out["error"] = "train_exception";
            out["error_detail"] = e.what();
        }
        catch(...){
            out["ok"] = false;
            out["error"] = "train_unknown_exception";
        }
        res.set_content(out.dump(2), "application/json");
    });
}
