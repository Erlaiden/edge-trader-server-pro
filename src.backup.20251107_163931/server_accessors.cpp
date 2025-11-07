#include "server_accessors.h"
#include "json.hpp"
#include <atomic>
#include <limits>
#include <fstream>
#include <cmath>

namespace etai {

using json = nlohmann::json;

// === Глобальное состояние (единый инстанс) ===
// Без NaN и мусора — валидные дефолты.
static std::atomic<double>     G_MODEL_THR{0.38};
static std::atomic<long long>  G_MODEL_MA{12};
static std::atomic<int>        G_FEAT_DIM{28};
static json                    G_CURRENT_MODEL = json::object();

// Последний инференс (телеметрия)
static std::atomic<double>     G_LAST_SCORE{0.0};
static std::atomic<double>     G_LAST_SIGMA{0.0};
static std::atomic<int>        G_LAST_SIGNAL{0}; // -1/0/1

// --- Threshold ---
double get_model_thr() { return G_MODEL_THR.load(std::memory_order_relaxed); }
void   set_model_thr(double v) { G_MODEL_THR.store(v, std::memory_order_relaxed); }

// --- MA length ---
long long get_model_ma_len() { return G_MODEL_MA.load(std::memory_order_relaxed); }
void      set_model_ma_len(long long v) { G_MODEL_MA.store(v, std::memory_order_relaxed); }

// --- Feature dimension (каноническая) ---
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
    json j; f >> j;
    return j.is_object()? j : json::object();
  } catch(...) { return json::object(); }
}

// --- Helpers: достать значения с диска безопасно ---
static inline int extract_feat_dim_from_disk(const json& disk, int def_feat_dim) {
  // приоритет: model.policy.feat_dim -> metrics.feat_cols -> дефолт
  try {
    if (disk.contains("policy") && disk["policy"].is_object()) {
      const json& p = disk["policy"];
      if (p.contains("feat_dim") && !p["feat_dim"].is_null()) {
        int d = p["feat_dim"].get<int>();
        if (d > 0 && d < 4096) return d;
      }
    }
  } catch(...) {}

  try {
    if (disk.contains("metrics") && disk["metrics"].is_object()) {
      const json& m = disk["metrics"];
      if (m.contains("feat_cols") && !m["feat_cols"].is_null()) {
        int d = m["feat_cols"].get<int>();
        if (d > 0 && d < 4096) return d;
      }
    }
  } catch(...) {}

  return def_feat_dim;
}

static inline double extract_thr_from_disk(const json& disk, double def_thr) {
  try {
    if (disk.contains("best_thr") && disk["best_thr"].is_number()) {
      double t = disk["best_thr"].get<double>();
      if (std::isfinite(t) && t > 1e-6 && t < 1.0) return t;
    }
  } catch(...) {}
  return def_thr;
}

static inline long long extract_ma_from_disk(const json& disk, long long def_ma) {
  try {
    if (disk.contains("ma_len") && disk["ma_len"].is_number_integer()) {
      long long m = disk["ma_len"].get<long long>();
      if (m > 0 && m < 100000) return m;
    }
  } catch(...) {}
  return def_ma;
}

// --- Startup init: читаем диск, выставляем атомики, гарантируем не-NaN ---
void init_model_atoms_from_disk(const char* path,
                                double def_thr,
                                long long def_ma,
                                int def_feat_dim)
{
  // Дефолты заранее
  set_model_thr(def_thr);
  set_model_ma_len(def_ma);
  set_feat_dim(def_feat_dim);
  set_current_model(json::object());

  // Читаем модель с диска, если есть
  json disk = safe_read_json_file(path);
  if (disk.is_object()) {
    set_current_model(disk);

    // Извлекаем агрегаты модели
    double    thr = extract_thr_from_disk(disk, def_thr);
    long long ma  = extract_ma_from_disk(disk,  def_ma);
    int       fd  = extract_feat_dim_from_disk(disk, def_feat_dim);

    // Последовательное и безопасное обновление атомиков
    set_model_thr(thr);
    set_model_ma_len(ma);
    set_feat_dim(fd);
  }

  // Сбрасываем телеметрию последнего инференса в нейтраль
  set_last_infer_score(0.0);
  set_last_infer_sigma(0.0);
  set_last_infer_signal(0);
}

// --- Last inference telemetry ---
double get_last_infer_score() { return G_LAST_SCORE.load(std::memory_order_relaxed); }
void   set_last_infer_score(double v) { G_LAST_SCORE.store(v, std::memory_order_relaxed); }

double get_last_infer_sigma() { return G_LAST_SIGMA.load(std::memory_order_relaxed); }
void   set_last_infer_sigma(double v) { G_LAST_SIGMA.store(v, std::memory_order_relaxed); }

int    get_last_infer_signal() { return G_LAST_SIGNAL.load(std::memory_order_relaxed); }
void   set_last_infer_signal(int s) { G_LAST_SIGNAL.store(s, std::memory_order_relaxed); }

} // namespace etai
