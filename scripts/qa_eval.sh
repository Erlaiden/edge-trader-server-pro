#!/usr/bin/env bash
set -euo pipefail

# A) Инференс по текущей модели
echo "== INFER =="
curl -sS "http://127.0.0.1:3000/api/infer?symbol=BTCUSDT&interval=15" \
| jq '{ok, signal, score, sigma, used_norm, feat_dim_used}'

# B) Быстрый скан порога в off-policy среде (нужно ETAI_ENABLE_TRAIN_ENV=1)
echo "== ENV THR SWEEP (tp=0.0044 sl=0.0016 fee=0.0002, steps=3000) =="
ETAI_ENABLE_TRAIN_ENV=1 ./build/edge_trader_server 3001 >/tmp/env.log 2>&1 &
EP=$!
sleep 1
function envq(){ curl -sS "http://127.0.0.1:3001/api/train_env?steps=3000&policy=model&fee=0.0002&tp=0.0044&sl=0.0016&thr=$1&atr=$2" \
| jq -r --arg thr "$1" --arg atr "$2" '[
    ($thr|tonumber),
    .winrate, .pf, .sharpe, .max_dd, .equity_final, (.params.e_lo//null), (.params.use_atr//false), (.skipped//0)
] | @tsv'; }

printf "thr\twinrate\tpf\tsharpe\tmax_dd\tequity\te_lo\tatr\tskipped\n"
for thr in 0.34 0.36 0.38 0.40; do
  for atr in 0 1; do
    echo -e "$(envq "$thr" "$atr")"
  done
done | column -t

# C) Вариант с контекстным E_LO
echo "== ENV E_LO gating check (e_lo=0.20) =="
curl -sS "http://127.0.0.1:3001/api/train_env?steps=3000&policy=model&fee=0.0002&tp=0.0044&sl=0.0016&thr=0.36&atr=1&elo=0.20" \
| jq '{thr:.params.thr, e_lo:.params.e_lo, winrate, pf, sharpe, max_dd, equity_final, skipped}'

kill "$EP" || true; rm -f /tmp/env.pid || true

# D) Прометеус-метрики модели
echo "== PROM METRICS =="
curl -sS "http://127.0.0.1:3000/metrics" \
| egrep -E 'edge_model_(thr|ma_len|feat_dim)|edge_reward_(avg|wctx)|edge_(sharpe|winrate|drawdown)' || true
