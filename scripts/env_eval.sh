#!/usr/bin/env bash
set -euo pipefail

# -------- Параметры (переопределяй через env) --------
SYMBOL="${SYMBOL:-BTCUSDT}"
INTERVAL="${INTERVAL:-15}"
STEPS="${STEPS:-3000}"
FEE="${FEE:-0.0002}"
TP="${TP:-0.004}"
SL="${SL:-0.0018}"

BASE_URL="http://127.0.0.1:3001"

log(){ printf '%s\n' "[env-eval] $*"; }

cd /opt/edge-trader-server

# -------- Старт env-сервера --------
log "starting env server on :3001"
ETAI_ENABLE_TRAIN_ENV=1 ./build/edge_trader_server 3001 >/tmp/env.log 2>&1 & echo $! >/tmp/env.pid

# Подождём готовность (до ~5 сек)
for i in $(seq 1 50); do
  if curl -sS "${BASE_URL}/api/health" >/dev/null 2>&1; then
    log "env server is up"
    break
  fi
  sleep 0.1
done
sleep 0.3

# -------- Функция прогона --------
run_case() {
  local policy="$1"
  local thr="${2:-}"
  local url="${BASE_URL}/api/train_env?steps=${STEPS}&policy=${policy}&fee=${FEE}&tp=${TP}&sl=${SL}"
  if [[ -n "$thr" ]]; then
    url="${url}&thr=${thr}"
  fi
  curl -sS "$url" | jq '{
    ok,
    steps,
    equity_final,
    winrate,
    pf,
    sharpe,
    max_dd,
    gate,
    policy
  }'
}

# -------- Прогоны --------
log "run: policy=model (TP=${TP}, SL=${SL}, FEE=${FEE}, STEPS=${STEPS})"
MODEL_JSON="$(run_case "model")"
printf '%s\n' "$MODEL_JSON" | jq .

log "run: policy=thr_only (тот же TP/SL для сравнения)"
THR_JSON="$(run_case "thr_only")"
printf '%s\n' "$THR_JSON" | jq .

# -------- Компактная сводка по двум кейсам (TSV) --------
log "summary (TSV)"
{
  printf '%s\n' "$MODEL_JSON" | jq -r '[("model"), (.steps // 0), (.winrate // 0), (.pf // 0), (.sharpe // 0), (.max_dd // 0), (.equity_final // 0)] | @tsv'
  printf '%s\n' "$THR_JSON"   | jq -r '[("thr_only"), (.steps // 0), (.winrate // 0), (.pf // 0), (.sharpe // 0), (.max_dd // 0), (.equity_final // 0)] | @tsv'
} | awk 'BEGIN{print "POLICY\tSTEPS\tWINRATE\tPF\tSHARPE\tMAX_DD\tEQUITY_FINAL"} {print}'

# -------- Стоп env --------
if [[ -f /tmp/env.pid ]]; then
  kill "$(cat /tmp/env.pid)" >/dev/null 2>&1 || true
  rm -f /tmp/env.pid
fi
log "done"
