#include "train_logic.h"
#include "server_accessors.h"
#include "ppo_pro.h"
#include "utils_data.h"
#include "json.hpp"
#include <armadillo>
#include <mutex>
#include <chrono>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <cmath>

using json = nlohmann::json;

namespace etai {
using arma::mat;

static std::mutex train_mutex;

static inline long long now_ms(){
    return static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

json run_train_pro_and_save(const std::string& symbol,
                            const std::string& interval,
                            int episodes,
                            double tp,
                            double sl,
                            int ma_len)
{
    std::lock_guard<std::mutex> lk(train_mutex);
    json out={{"ok",false},{"error",nullptr},{"error_detail",nullptr},
              {"metrics",json::object()},{"model_path",nullptr}};

    try{
        // 1) X/y лишь для фиксации feat_dim (y — заглушка)
        arma::mat X,y;
        if(!etai::load_cached_xy(symbol,interval,X,y))
            throw std::runtime_error("failed to load/build XY cache");

        // 2) raw OHLCV (режем до 6 колонок, N>=300 внутри лоадера)
        arma::mat raw;
        if(!etai::load_raw_ohlcv(symbol,interval,raw))
            throw std::runtime_error("failed to load raw OHLCV");

        std::cout<<"[TRAIN] PPO_PRO rows="<<raw.n_rows
                 <<" raw_cols="<<raw.n_cols
                 <<" feat_cols="<<X.n_cols
                 <<" episodes="<<episodes
                 <<" tp="<<tp<<" sl="<<sl
                 <<" ma="<<ma_len<<std::endl;

        // 3) Запуск тренера
        json trainer = etai::trainPPO_pro(raw,nullptr,nullptr,nullptr,episodes,tp,sl,ma_len);

        // ---- ЖЁСТКАЯ ПРОВЕРКА УСПЕХА ----
        const bool ok_tr = trainer.value("ok", false);
        if(!ok_tr){
            out["ok"]=false;
            out["error"]="trainer_failed";
            out["error_detail"]=trainer.value("error_detail", trainer.value("error","unknown"));
            out["trainer"]=trainer; // отдадим полный ответ тренера для диагностики
            return out;
        }

        // 4) Забираем best_thr с верхнего уровня + metrics «как есть»
        double best_thr = trainer.value("best_thr",0.5);
        if(!(std::isfinite(best_thr)&&best_thr>0.0&&best_thr<1.0)) best_thr=0.5;

        json metrics = (trainer.contains("metrics") && trainer["metrics"].is_object())
                         ? trainer["metrics"] : json::object();

        // На всякий случай дополнительно положим ключевые поля, если их нет
        if(!metrics.contains("best_thr"))    metrics["best_thr"]=best_thr;
        if(!metrics.contains("feat_cols"))   metrics["feat_cols"]=(int)X.n_cols;

        // feat_version берём из тренера, если он его вернул; иначе ставим 3
        int feat_version = trainer.value("feat_version", 3);
        int feat_dim     = metrics.value("feat_cols", (int)X.n_cols);

        // 5) Сборка модели
        json model={
            {"version",3},
            {"schema","ppo_pro_v1"},
            {"mode","pro"},
            {"best_thr",best_thr},
            {"ma_len",ma_len},
            {"tp",tp},
            {"sl",sl},
            {"build_ts",now_ms()},
            {"policy",{
                {"feat_dim",feat_dim},
                {"feat_version",feat_version}
            }},
            {"metrics",metrics}
        };

        // 6) Сохранение
        std::string path="cache/models/"+symbol+"_"+interval+"_ppo_pro.json";
        {
            std::ofstream ofs(path);
            if(!ofs) throw std::runtime_error("failed to open model file for write: "+path);
            ofs<<model.dump(2);
        }

        // 7) Атомы — в экспортёр метрик
        etai::set_model_thr(best_thr);
        etai::set_model_ma_len(ma_len);
        etai::set_current_model(model);

        // 8) Ответ
        out["ok"]=true;
        out["best_thr"]=best_thr;
        out["model_path"]=path;
        out["metrics"]=metrics;

        return out;
    }catch(const std::exception& e){
        out["ok"]=false;
        out["error"]="train_pro_exception";
        out["error_detail"]=e.what();
        return out;
    }catch(...){
        out["ok"]=false;
        out["error"]="train_pro_unknown";
        out["error_detail"]="unknown error";
        return out;
    }
}

} // namespace etai
