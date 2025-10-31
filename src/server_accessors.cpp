#include "server_accessors.h"
#include "json.hpp"
#include <atomic>
#include <limits>
#include <fstream>
#include <cmath>

namespace etai {

using json = nlohmann::json;

// === Глобальное состояние (единый инстанс) ===
// СРАЗУ корректные дефолты, без NaN:
static std::atomic<double>    G_MODEL_THR{0.38};
static std::atomic<long long> G_MODEL_MA{12};
static std::atomic<int>       G_FEAT_DIM{28};
static json                   G_CURRENT_MODEL = json::object();

// --- Threshold ---
double get_model_thr() { return G_MODEL_THR.load(std::memory_order_relaxed); }
void   set_model_thr(double v) { G_MODEL_THR.store(v, std::memory_order_relaxed); }

// --- MA length ---
long long get_model_ma_len() { return G_MODEL_MA.load(std::memory_order_relaxed); }
void      set_model_ma_len(long long v) { G_MODEL_MA.store(v, std::memory_order_relaxed); }

// --- Feature dimension ---
int  get_feat_dim() { return G_FEAT_DIM.load(std::memory_order_relaxed); }
void set_feat_dim(int d) { G_FEAT_DIM.store(d, std::memory_order_relaxed); }

// --- Current model JSON ---
json get_current_model() { return G_CURRENT_MODEL; }
void set_current_model(const json& j) { G_CURRENT_MODEL = j; }

// --- Safe JSON read ---
static inline json safe_read_json_file(const char* p){
    try {
        std::ifstream f(p);
        if(!f.good()) return json::object();
        json j; f >> j; return j.is_object()? j : json::object();
    } catch(...) { return json::object(); }
}

// --- Startup init: читаем диск, выставляем атомики, гарантируем не-NaN ---
void init_model_atoms_from_disk(const char* path,
                                double def_thr,
                                long long def_ma,
                                int def_feat_dim)
{
    // выставим дефолты на всякий случай заранее
    set_model_thr(def_thr);
    set_model_ma_len(def_ma);
    set_feat_dim(def_feat_dim);
    set_current_model(json::object());

    json disk = safe_read_json_file(path);
    if(disk.is_object()){
        set_current_model(disk);
        // предпочитаем значения из файла, но санитизируем
        double thr = disk.value("best_thr", def_thr);
        long long ma = disk.value("ma_len", def_ma);
        if(!std::isfinite(thr)) thr = def_thr;
        if(ma <= 0) ma = def_ma;
        set_model_thr(thr);
        set_model_ma_len(ma);
    }
}

} // namespace etai
