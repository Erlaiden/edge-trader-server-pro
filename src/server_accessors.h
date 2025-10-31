#pragma once
#include <cstdint>
#include "json.hpp"

namespace etai {

// --- Threshold ---
double        get_model_thr();
void          set_model_thr(double v);

// --- MA length ---
long long     get_model_ma_len();
void          set_model_ma_len(long long v);

// --- Feature dimension (canonical) ---
int           get_feat_dim();
void          set_feat_dim(int d);

// --- Back-compat aliases expected by routes/metrics.cpp ---
inline int    get_model_feat_dim() { return get_feat_dim(); }
inline void   set_model_feat_dim(int d) { set_feat_dim(d); }

// --- Current model JSON ---
nlohmann::json get_current_model();
void           set_current_model(const nlohmann::json& j);

// --- Startup initialization from disk (with safe defaults) ---
void init_model_atoms_from_disk(const char* path,
                                double def_thr,
                                long long def_ma,
                                int def_feat_dim);

} // namespace etai
