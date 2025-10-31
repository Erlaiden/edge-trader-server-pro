#include "agent_layer.h"
#include <numeric>
#include <algorithm>
#include "agents_factory.h"

namespace etai {

AgentLayer::AgentLayer() {
    agents_.emplace_back("long",       make_agent_long());
    agents_.emplace_back("short",      make_agent_short());
    agents_.emplace_back("flat",       make_agent_flat());
    agents_.emplace_back("breakout",   make_agent_breakout());
    agents_.emplace_back("correction", make_agent_correction());
}

AgentSummary AgentLayer::decide_all(const arma::rowvec& features, double thr) {
    AgentSummary sum;
    if (features.n_cols == 0 || agents_.empty()) return sum;

    std::vector<int> signals;
    std::vector<double> confs;

    for (auto& kv : agents_) {
        const std::string& name = kv.first;
        auto& agent = kv.second;

        int sig = 0;
        double conf = 0.0;
        try {
            sig = agent->decide(features, thr);
            conf = agent->confidence();
        } catch (...) {
            sig = 0;
            conf = 0.0;
        }
        signals.push_back(sig);
        confs.push_back(conf);
        sum.agents[name] = {sig, conf};
    }

    const int sum_signals = std::accumulate(signals.begin(), signals.end(), 0);
    const int non_zero = static_cast<int>(std::count_if(signals.begin(), signals.end(),
                                                        [](int s){ return s != 0; }));

    if (sum_signals > 0)      sum.final_signal = +1;
    else if (sum_signals < 0) sum.final_signal = -1;
    else                      sum.final_signal = 0;

    // Конфликт: считаем только по ненулевым сигналам
    if (non_zero > 0) {
        if (sum.final_signal == 0) {
            sum.conflict_ratio = 1.0; // есть ненулевые, но итог — нейтраль
        } else {
            int same_dir = 0;
            for (int s : signals) if (s != 0 && s == sum.final_signal) ++same_dir;
            int conflict = non_zero - same_dir;
            sum.conflict_ratio = static_cast<double>(conflict) / static_cast<double>(non_zero);
        }
    } else {
        sum.conflict_ratio = 0.0;
    }

    // Уверенность:
    // - если финал нейтральный — усредняем по всем агентам
    // - иначе — усредняем только тех, кто поддержал итог
    if (sum.final_signal == 0) {
        sum.final_confidence = confs.empty() ? 0.0
            : std::accumulate(confs.begin(), confs.end(), 0.0) / static_cast<double>(confs.size());
    } else {
        double acc = 0.0; int cnt = 0;
        for (std::size_t i = 0; i < signals.size(); ++i) {
            if (signals[i] == sum.final_signal) { acc += confs[i]; ++cnt; }
        }
        sum.final_confidence = cnt ? (acc / static_cast<double>(cnt)) : 0.0;
    }

    return sum;
}

} // namespace etai
