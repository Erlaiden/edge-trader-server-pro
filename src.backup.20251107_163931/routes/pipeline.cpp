// routes/pipeline.cpp
// Конвейер: backfill -> clean -> fill gaps (15m) -> train -> infer snapshot.
// Порт 3000. JSON-in / JSON-out. Никаких новых портов.

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>
#include <array>
#include <stdexcept>

#include <httplib.h>
#include "json.hpp"

namespace {

using json = nlohmann::json;

std::string run_cmd(const std::string& cmd) {
    std::array<char, 4096> buf{};
    std::string out;
    FILE* pipe = popen((cmd + " 2>&1").c_str(), "r");
    if (!pipe) return "";
    while (fgets(buf.data(), buf.size(), pipe)) out.append(buf.data());
    pclose(pipe);
    return out;
}

// есть ли дырки по 15m (шаг != 900000)
bool has_15m_gaps(const std::string& clean_path) {
    std::string awk = "awk -F, 'NR>1 {d=$1-prev; if(d!=900000) bad++; prev=$1} NR==1 {prev=$1} END{exit (bad?1:0)}' " + clean_path;
    int rc = std::system(awk.c_str());
    if (WIFEXITED(rc)) return (WEXITSTATUS(rc) != 0);
    return true; // подстраховка
}

long rows_in_file(const std::string& path) {
    std::string out = run_cmd("wc -l < " + path + " | tr -d ' \\n'");
    try { return std::stol(out); } catch (...) { return -1; }
}

} // namespace

inline void register_pipeline_routes(httplib::Server& svr) {
    // POST /api/pipeline/prepare_train
    // body: {symbol, months?, interval?, tp?, sl?, ma?, episodes?}
    svr.Post("/api/pipeline/prepare_train", [](const httplib::Request& req, httplib::Response& res) {
        using json = nlohmann::json;
        json resp; resp["ok"] = false;

        json in;
        try { in = json::parse(req.body.empty() ? "{}" : req.body); }
        catch (...) { res.status=400; resp["error"]="invalid_json"; res.set_content(resp.dump(2),"application/json"); return; }

        const std::string sym = in.value("symbol", "");
        if (sym.empty()) { res.status=400; resp["error"]="missing_symbol"; res.set_content(resp.dump(2),"application/json"); return; }

        const int    months   = in.value("months",   6);
        const int    interval = in.value("interval", 15);
        const double tp       = in.value("tp",       0.006);
        const double sl       = in.value("sl",       0.0024);
        const int    ma       = in.value("ma",       12);
        const int    episodes = in.value("episodes", 120);

        json steps = json::array();
        auto step_ok=[&](const std::string& name, const json& extra=json::object()){ json j={{"step",name},{"ok",true}}; for(auto it=extra.begin(); it!=extra.end(); ++it) j[it.key()]=it.value(); steps.push_back(j); };
        auto step_fail=[&](const std::string& name, const std::string& err, const json& extra=json::object()){ json j={{"step",name},{"ok",false},{"error",err}}; for(auto it=extra.begin(); it!=extra.end(); ++it) j[it.key()]=it.value(); steps.push_back(j); };

        // 1) Backfill всех ТФ через локальный /api/backfill
        try{
            httplib::Client cli("127.0.0.1",3000); cli.set_read_timeout(300,0);
            std::string path = "/api/backfill?symbol="+sym+"&months="+std::to_string(months)+"&which=15,60,240,1440";
            auto r = cli.Get(path.c_str());
            if(!r || r->status!=200){ step_fail("backfill","http_error",{{"status", r? r->status:0}}); res.status=500; resp["steps"]=steps; res.set_content(resp.dump(2),"application/json"); return; }
            json back = json::parse(r->body);
            step_ok("backfill", {{"intervals", back.value("intervals", json::array())}});
        } catch(const std::exception& e){ step_fail("backfill", e.what()); res.status=500; resp["steps"]=steps; res.set_content(resp.dump(2),"application/json"); return; }

        // 2) CLEAN из RAW (все ТФ)
        {
            std::string out = run_cmd("bash scripts/clean_from_raw.sh '"+sym+"'");
            long r15 = rows_in_file("cache/clean/"+sym+"_15.csv");
            if (r15 < 0){ step_fail("clean","clean_15_missing",{{"stdout",out}}); res.status=500; resp["steps"]=steps; res.set_content(resp.dump(2),"application/json"); return; }
            step_ok("clean", {{"rows15", r15}});
        }

        // 3) Автозаполнение дыр 15m при необходимости
        {
            const std::string clean15 = "cache/clean/"+sym+"_15.csv";
            if (has_15m_gaps(clean15)) {
                std::string out = run_cmd("bash scripts/fill_gaps_bybit_15m.sh '"+sym+"'");
                long r15 = rows_in_file(clean15);
                if (has_15m_gaps(clean15)){ step_fail("fill_gaps_15m","gaps_remain",{{"rows15",r15},{"stdout",out}}); res.status=500; resp["steps"]=steps; res.set_content(resp.dump(2),"application/json"); return; }
                step_ok("fill_gaps_15m", {{"rows15", r15}});
            } else {
                step_ok("fill_gaps_15m", {{"note","no_gaps"}});
            }
        }

        // 4) Верификация минимума строк на 15m
        {
            long r15 = rows_in_file("cache/clean/"+sym+"_15.csv");
            if (r15 < 300){ step_fail("verify_rows_15m","too_few_rows",{{"rows15",r15}}); res.status=400; resp["steps"]=steps; res.set_content(resp.dump(2),"application/json"); return; }
            step_ok("verify_rows_15m", {{"rows15", r15}});
        }

        // 5) TRAIN (fetch=0 cleanup=0 antimanip=1)
        json train_j;
        try{
            httplib::Client cli("127.0.0.1",3000); cli.set_read_timeout(600,0);
            std::ostringstream p; p << "/api/train?symbol="<<sym<<"&interval="<<interval<<"&episodes="<<episodes
                                    <<"&tp="<<tp<<"&sl="<<sl<<"&ma="<<ma<<"&fetch=0&cleanup=0&antimanip=1";
            auto r = cli.Get(p.str().c_str());
            if(!r || r->status!=200){ step_fail("train","http_error",{{"status", r? r->status:0}}); res.status=500; resp["steps"]=steps; res.set_content(resp.dump(2),"application/json"); return; }
            train_j = json::parse(r->body);
            if(!train_j.value("ok", false)){ step_fail("train","train_not_ok",{{"train",train_j}}); res.status=500; resp["steps"]=steps; res.set_content(resp.dump(2),"application/json"); return; }
            step_ok("train", {{"best_thr",train_j.value("best_thr",0.0)},{"val_winrate",train_j.value("val_winrate",0.0)},{"val_sharpe",train_j.value("val_sharpe",0.0)}});
        } catch(const std::exception& e){ step_fail("train", e.what()); res.status=500; resp["steps"]=steps; res.set_content(resp.dump(2),"application/json"); return; }

        // 6) Снимок для UI
        json infer_j;
        try{
            httplib::Client cli("127.0.0.1",3000); cli.set_read_timeout(60,0);
            std::ostringstream p; p << "/api/infer?symbol="<<sym<<"&interval="<<interval<<"&htf=60,240,1440";
            auto r = cli.Get(p.str().c_str());
            if (r && r->status==200){
                infer_j = json::parse(r->body);
                step_ok("infer_snapshot", {{"signal", infer_j.value("signal","NEUTRAL")}, {"score15", infer_j.value("score15",0.0)}, {"thr", infer_j.value("thr",0.0)}});
            } else {
                step_fail("infer_snapshot","http_error",{{"status", r? r->status:0}});
            }
        } catch (...) { step_fail("infer_snapshot","exception"); }

        resp["ok"]=true; resp["symbol"]=sym; resp["months"]=months; resp["interval"]=interval; resp["train"]=train_j; resp["infer"]=infer_j; resp["steps"]=steps;
        res.status=200; res.set_content(resp.dump(2),"application/json");
    });
}
