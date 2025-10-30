#include "utils_data.h"
#include "json.hpp"
#include <armadillo>
#include <fstream>
#include <sstream>
#include <filesystem>

using nlohmann::json;
namespace fs = std::filesystem;

namespace etai {

// Отчёт по здоровью данных: быстрые проверки наличия и строк
json data_health_report(const std::string& symbol, const std::string& interval) {
    const std::string path = "cache/" + symbol + "_" + interval + ".csv";
    json j;
    j["symbol"]   = symbol;
    j["interval"] = interval;
    j["ok"]       = fs::exists(path);
    if (!j["ok"]) return j;

    std::ifstream in(path);
    int rows = 0;
    std::string line;
    while (std::getline(in, line)) rows++;
    j["rows"]  = rows > 0 ? rows - 1 : 0; // без заголовка
    j["ts_min"] = nullptr;
    j["ts_max"] = nullptr;
    j["dups"]   = 0;
    j["gaps"]   = 0;
    return j;
}

// Загрузка кэша признаков и таргета
bool load_cached_xy(const std::string& symbol,
                    const std::string& interval,
                    arma::mat& X,
                    arma::mat& y) {
    const std::string xfile = "cache/X_" + symbol + "_" + interval + ".csv";
    const std::string yfile = "cache/y_" + symbol + "_" + interval + ".csv";
    if (!fs::exists(xfile) || !fs::exists(yfile)) return false;

    try {
        X.load(xfile, arma::csv_ascii);
        y.load(yfile, arma::csv_ascii);
        return X.n_rows == y.n_rows && X.n_rows > 0;
    } catch (...) {
        return false;
    }
}

// Реализация, которой не хватало линковщику
// Сводка по всем CSV в cache/, используется в health_ai и train_logic
json get_data_health() {
    json out = json::object();
    try {
        if (!fs::exists("cache")) {
            out["ok"] = false;
            out["error"] = "cache_dir_missing";
            return out;
        }
        for (const auto& p : fs::directory_iterator("cache")) {
            if (!p.is_regular_file()) continue;
            if (p.path().extension() != ".csv") continue;
            const std::string fname = p.path().filename().string();
            json f;
            f["ok"]   = fs::exists(p.path());
            f["size"] = f["ok"] ? static_cast<uint64_t>(fs::file_size(p.path())) : 0ULL;

            // Быстрая оценка количества строк
            try {
                std::ifstream in(p.path());
                std::string line;
                int rows = 0;
                while (std::getline(in, line)) rows++;
                f["rows"] = rows > 0 ? rows - 1 : 0;
            } catch (...) {
                f["rows"] = 0;
            }
            out[fname] = f;
        }
        out["ok"] = true;
    } catch (const std::exception& e) {
        out["ok"] = false;
        out["error"] = e.what();
    }
    return out;
}

} // namespace etai
