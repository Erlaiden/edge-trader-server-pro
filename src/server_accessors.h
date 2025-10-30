#pragma once
#include "json.hpp"
#include <atomic>
#include <string>

namespace etai {

// --- threshold ---
double     get_model_thr();
void       set_model_thr(double v);

// --- MA length ---
long long  get_model_ma_len();
void       set_model_ma_len(long long v);

// --- feat_dim (из policy) ---
int        get_model_feat_dim();
void       set_model_feat_dim(int v);

// --- текущая модель (JSON) ---
const nlohmann::json& get_current_model();
void                  set_current_model(const nlohmann::json& m);

// --- инициализация атомов из файла модели (по умолчанию BTCUSDT/15) ---
void init_model_atoms_from_disk(const std::string& symbol = "BTCUSDT",
                                const std::string& interval = "15");

// (опционально, если используется где-то ещё)
nlohmann::json get_data_health();

} // namespace etai
