#include "server_accessors.h"
#include <atomic>
#include <fstream>
#include <filesystem>
#include "json.hpp"
#include "utils_model.h"  // safe_read_json_file(...) если у тебя уже есть

namespace fs = std::filesystem;
using nlohmann::json;

namespace etai {

  // --- атомарные параметры модели (дефолты безопасные) ---
  std::atomic<double>     MODEL_THR{0.01};
  std::atomic<long long>  MODEL_MA_LEN{12};
  std::atomic<int>        MODEL_FEAT_DIM{8};

  // --- текущая модель целиком ---
  static json CURRENT_MODEL = json::object();

  // --- threshold ---
  double get_model_thr()                { return MODEL_THR.load(); }
  void   set_model_thr(double v)        { MODEL_THR.store(v); }

  // --- MA len ---
  long long get_model_ma_len()          { return MODEL_MA_LEN.load(); }
  void      set_model_ma_len(long long v){ MODEL_MA_LEN.store(v); }

  // --- feat_dim ---
  int  get_model_feat_dim()             { return MODEL_FEAT_DIM.load(); }
  void set_model_feat_dim(int v)        { MODEL_FEAT_DIM.store(v); }

  // --- current model ---
  const json& get_current_model()       { return CURRENT_MODEL; }
  void        set_current_model(const json& m)
  {
    CURRENT_MODEL = m;

    // Поддержим согласованность атомов при обновлении модели
    try {
      if (m.contains("best_thr")) set_model_thr(m.at("best_thr").get<double>());
    } catch (...) {}
    try {
      if (m.contains("ma_len"))   set_model_ma_len(m.at("ma_len").get<long long>());
    } catch (...) {}
    try {
      if (m.contains("policy") && m.at("policy").contains("feat_dim")) {
        set_model_feat_dim(m.at("policy").at("feat_dim").get<int>());
      }
    } catch (...) {}
  }

  // --- загрузка json с диска (через твою безопасную утилиту) ---
  static json read_json_file(const std::string& path)
  {
    try {
      return safe_read_json_file(path);
    } catch (...) {
      return json::object();
    }
  }

  // --- инициализация атомов из файла модели ---
  void init_model_atoms_from_disk(const std::string& symbol, const std::string& interval)
  {
    // Политика именования: cache/models/<SYMBOL>_<INTERVAL>_ppo_pro.json
    const std::string fname = "cache/models/" + symbol + "_" + interval + "_ppo_pro.json";
    if (!fs::exists(fname)) {
      // файла нет — оставляем дефолты и пустую CURRENT_MODEL
      return;
    }

    json disk = read_json_file(fname);
    if (disk.is_object() && disk.contains("model")) {
      // некоторые ручки хранят под .model — нормализуем
      disk = disk["model"];
    }

    if (!disk.is_object()) return;

    // Дополняем policy, если нужно
    if (!disk.contains("policy")) disk["policy"] = json::object();

    // Обновляем CURRENT_MODEL и атомы
    set_current_model(disk);
  }

  // Заглушка-декларация, если где-то зовут (реализация в utils_data.cpp)
  nlohmann::json get_data_health();

} // namespace etai
