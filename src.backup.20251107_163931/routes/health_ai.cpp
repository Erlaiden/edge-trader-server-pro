#include "../httplib.h"
#include "json.hpp"
#include "../server_accessors.h"
#include "../utils_model.h"
#include "../utils_data.h"
#include <fstream>
#include <string>
#include <optional>
#include <vector>
#include <cstdlib>

#if __has_include(<filesystem>)
  #include <filesystem>
  namespace fs = std::filesystem;
#else
  #error "C++17 filesystem is required"
#endif

#if __has_include("../agents.h")
  #include "../agents.h"
  #define ETAI_HAS_AGENTS 1
#else
  #define ETAI_HAS_AGENTS 0
#endif

using json = nlohmann::json;

// ---------- paths ----------
static fs::path project_root() {
    if (const char* env = std::getenv("ETAI_ROOT")) {
        fs::path p(env);
        if (fs::exists(p / "cache")) return p;
    }
    fs::path cwd = fs::current_path();
    if (fs::exists(cwd / "cache")) return cwd;
    fs::path opt("/opt/edge-trader-server");
    if (fs::exists(opt / "cache")) return opt;
    return cwd;
}
static fs::path cache_dir()  { return project_root() / "cache"; }
static fs::path models_dir() { return cache_dir() / "models"; }

// ---------- helpers ----------
static inline json null_model_short() {
    return json{
        {"best_thr", nullptr}, {"ma_len", nullptr}, {"tp", nullptr},
        {"sl", nullptr}, {"feat_dim", nullptr}, {"version", nullptr},
        {"symbol", nullptr}, {"interval", nullptr}, {"schema", nullptr},
        {"mode", nullptr}, {"model_path", nullptr}
    };
}
static std::optional<json> read_json_file(const fs::path& p) {
    try { std::ifstream ifs(p); if (!ifs.good()) return std::nullopt; json j; ifs >> j; return j; }
    catch (...) { return std::nullopt; }
}
static std::optional<fs::path> expected_model_path(const std::string& symbol, const std::string& interval) {
    fs::path p = models_dir() / (symbol + "_" + interval + "_ppo_pro.json");
    if (fs::exists(p) && fs::is_regular_file(p)) return p; return std::nullopt;
}
static std::optional<fs::path> latest_model_on_disk() {
    try {
        fs::path dir = models_dir();
        if (!fs::exists(dir) || !fs::is_directory(dir)) return std::nullopt;
        std::optional<fs::path> best; fs::file_time_type best_ts{};
        for (const auto& e : fs::directory_iterator(dir)) {
            if (!e.is_regular_file() || e.path().extension()!=".json") continue;
            auto ts = fs::last_write_time(e);
            if (!best || ts > best_ts) { best = e.path(); best_ts = ts; }
        } return best;
    } catch (...) { return std::nullopt; }
}
static std::optional<std::pair<std::string,std::string>> symbol_interval_from_filename(const fs::path& p) {
    try {
        auto name = p.filename().string();
        auto pos1 = name.find('_');
        auto pos2 = name.find('_', (pos1==std::string::npos)?0:pos1+1);
        if (pos1!=std::string::npos && pos2!=std::string::npos) {
            std::string sym = name.substr(0, pos1);
            std::string ivl = name.substr(pos1+1, pos2-pos1-1);
            if (!sym.empty() && !ivl.empty()) return std::make_pair(sym,ivl);
        }
    } catch (...) {}
    return std::nullopt;
}
static void fill_from_ram(json& dst, const json& m) {
    auto put = [&](const char* k){ if (m.contains(k)) dst[k]=m[k]; };
    put("best_thr"); put("ma_len"); put("tp"); put("sl");
    put("version"); put("symbol"); put("interval"); put("schema"); put("mode");
    try { if (m.contains("policy") && m["policy"].contains("feat_dim"))
        dst["feat_dim"]=m["policy"]["feat_dim"]; } catch (...) {}
    if (m.contains("model_path") && m["model_path"].is_string())
        dst["model_path"]=m["model_path"];
}
// overwrite: ядро всегда из диска
static void fill_from_json_overwrite_core(json& dst, const json& j, const fs::path& p) {
    dst["model_path"] = p.string();
    auto overwrite = [&](const char* k){ if (j.contains(k)) dst[k]=j[k]; };
    overwrite("best_thr"); overwrite("ma_len"); overwrite("tp"); overwrite("sl");
    overwrite("version"); overwrite("schema"); overwrite("mode");
    overwrite("symbol"); overwrite("interval");
    try {
        if (j.contains("policy") && j["policy"].contains("feat_dim"))
            dst["feat_dim"] = j["policy"]["feat_dim"];
    } catch (...) {}
}

// ---------- CONTEXT (fresh model only) ----------
struct Ctx { std::string symbol; std::string interval; std::string source; };

// RAM -> RAM path(JSON/filename) -> latest disk
static Ctx compute_context_only_model() {
    Ctx ctx;
    try {
        auto m = etai::get_current_model();
        std::string msym, mivl, mpath;
        if (m.contains("symbol") && m["symbol"].is_string())   msym = m["symbol"].get<std::string>();
        if (m.contains("interval") && m["interval"].is_string()) mivl = m["interval"].get<std::string>();
        if (m.contains("model_path") && m["model_path"].is_string()) mpath = m["model_path"].get<std::string>();
        if (!msym.empty() && !mivl.empty()) { ctx.symbol=msym; ctx.interval=mivl; ctx.source="ram"; return ctx; }
        if (!mpath.empty()) {
            fs::path p = project_root() / mpath;
            if (fs::exists(p)) {
                if (auto j = read_json_file(p)) {
                    if (j->contains("symbol") && (*j)["symbol"].is_string())   msym = (*j)["symbol"].get<std::string>();
                    if (j->contains("interval") && (*j)["interval"].is_string()) mivl = (*j)["interval"].get<std::string>();
                }
                if (msym.empty() || mivl.empty()) {
                    if (auto si = symbol_interval_from_filename(p)) {
                        if (msym.empty()) msym = si->first;
                        if (mivl.empty()) mivl = si->second;
                    }
                }
                if (!msym.empty() && !mivl.empty()) { ctx.symbol=msym; ctx.interval=mivl; ctx.source="ram-path"; return ctx; }
            }
        }
    } catch (...) {}

    if (auto lp = latest_model_on_disk()) {
        if (auto j = read_json_file(*lp)) {
            std::string sym, ivl;
            if (j->contains("symbol") && (*j)["symbol"].is_string())
                sym = (*j)["symbol"].get<std::string>();
            if (j->contains("interval") && (*j)["interval"].is_string())
                ivl = (*j)["interval"].get<std::string>();
            if (sym.empty() || ivl.empty()) {
                if (auto si = symbol_interval_from_filename(*lp)) {
                    if (sym.empty()) sym = si->first;
                    if (ivl.empty()) ivl = si->second;
                }
            }
            if (!sym.empty() && !ivl.empty()) { ctx.symbol=sym; ctx.interval=ivl; ctx.source="latest"; return ctx; }
        }
    }
    return ctx;
}

void register_health_ai(httplib::Server& svr) {
    svr.Get("/api/health/ai", [](const httplib::Request&, httplib::Response& res) {
        json out; out["ok"]=true;

        // agents: без символа, если idle
        json ag = json{{"ok", false}};
        #if ETAI_HAS_AGENTS
        try { ag = etai::get_agent_public(); } catch (...) { ag=json{{"ok", false}}; }
        if (!ag.value("running", false)) ag["symbol"]=nullptr;
        #endif
        out["agents"] = ag;

        // context = только модель
        Ctx ctx = compute_context_only_model();
        out["context"] = json{
            {"symbol",   ctx.symbol.empty()?nullptr:json(ctx.symbol)},
            {"interval", ctx.interval.empty()?nullptr:json(ctx.interval)},
            {"source",   ctx.source.empty()?"unknown":ctx.source}
        };

        // data health — фильтр по контексту
        try {
            json dh = etai::get_data_health();
            if (!ctx.symbol.empty() && dh.contains("data") && dh["data"].is_array()) {
                json filtered = json::array();
                for (const auto& item : dh["data"]) {
                    try {
                        if (item.contains("symbol") && item["symbol"].is_string()
                         && item["symbol"].get<std::string>() == ctx.symbol) filtered.push_back(item);
                    } catch (...) {}
                }
                dh["data"] = filtered;
            } else {
                dh["data"] = json::array();
            }
            out["data"] = dh;
            out["data_ok"] = dh.value("ok", false);
        } catch (...) {
            out["data"] = json{{"ok", false}};
            out["data_ok"] = false;
        }

        // model — RAM -> перезапись ядра из диска -> фоллбэк latest
        json ms = null_model_short();
        try { json m = etai::get_current_model(); fill_from_ram(ms, m); } catch (...) {}

        // если есть model_path RAM — дочитаем и ПЕРЕЗАПИШЕМ ядро
        if (ms.contains("model_path") && ms["model_path"].is_string()) {
            fs::path p = project_root() / ms["model_path"].get<std::string>();
            if (fs::exists(p)) {
                if (auto j = read_json_file(p)) fill_from_json_overwrite_core(ms, *j, p);
                if ((!ms.contains("symbol") || ms["symbol"].is_null()
                  || !ms.contains("interval") || ms["interval"].is_null())) {
                    if (auto si = symbol_interval_from_filename(p)) {
                        if (!ms.contains("symbol") || ms["symbol"].is_null())   ms["symbol"] = si->first;
                        if (!ms.contains("interval") || ms["interval"].is_null()) ms["interval"] = si->second;
                    }
                }
            }
        }

        // если контекст известен — дочитаем именно эту модель и ПЕРЕЗАПИШЕМ ядро
        if (!ctx.symbol.empty() && !ctx.interval.empty()) {
            if (auto ep = expected_model_path(ctx.symbol, ctx.interval)) {
                if (auto j = read_json_file(*ep)) fill_from_json_overwrite_core(ms, *j, *ep);
            }
        }

        // === ЖЁСТКО: символ/интервал должны соответствовать контексту ===
        if (!ctx.symbol.empty())   ms["symbol"] = ctx.symbol;
        if (!ctx.interval.empty()) ms["interval"] = ctx.interval;

        // фоллбэк — latest disk
        bool need_disk = (!ms.contains("tp") || ms["tp"].is_null()
                       || !ms.contains("sl") || ms["sl"].is_null()
                       || !ms.contains("ma_len") || ms["ma_len"].is_null()
                       || !ms.contains("best_thr") || ms["best_thr"].is_null()
                       || !ms.contains("feat_dim") || ms["feat_dim"].is_null()
                       || !ms.contains("model_path") || ms["model_path"].is_null());
        if (need_disk) {
            if (auto lp = latest_model_on_disk()) {
                if (auto j = read_json_file(*lp)) fill_from_json_overwrite_core(ms, *j, *lp);
                if ((!ms.contains("symbol") || ms["symbol"].is_null()
                  || !ms.contains("interval") || ms["interval"].is_null())) {
                    if (auto si = symbol_interval_from_filename(*lp)) {
                        if (!ms.contains("symbol") || ms["symbol"].is_null())   ms["symbol"] = si->first;
                        if (!ms.contains("interval") || ms["interval"].is_null()) ms["interval"] = si->second;
                    }
                }
            }
            // и снова подстрахуемся контекстом
            if (!ctx.symbol.empty())   ms["symbol"] = ctx.symbol;
            if (!ctx.interval.empty()) ms["interval"] = ctx.interval;
        }

        // атомики и согласование
        try {
            if (ms.contains("best_thr") && !ms["best_thr"].is_null())
                out["model_thr"] = ms["best_thr"];
            else
                out["model_thr"] = etai::get_model_thr();
        } catch (...) {}
        try {
            out["model_ma_len"] = etai::get_model_ma_len();
            if (!ms.contains("ma_len") || ms["ma_len"].is_null()) ms["ma_len"]=out["model_ma_len"];
        } catch (...) {}
        try {
            out["model_feat_dim"] = etai::get_model_feat_dim();
            if (!ms.contains("feat_dim") || ms["feat_dim"].is_null()) ms["feat_dim"]=out["model_feat_dim"];
        } catch (...) { out["model_feat_dim"]=nullptr; }

        out["model"] = ms;
        res.status=200; res.set_content(out.dump(2), "application/json");
    });
}
