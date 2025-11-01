#!/usr/bin/env bash
set -euo pipefail

# -------- Базовые параметры (можно переопределять через env) --------
SYMBOL="${SYMBOL:-BTCUSDT}"
INTERVAL="${INTERVAL:-15}"
STEPS="${STEPS:-3000}"
FEE="${FEE:-0.0002}"
TP="${TP:-0.004}"
SL="${SL:-0.0018}"

# Сетки
AGGR_LIST=${AGGR_LIST:-"0.10 0.15 0.20"}
ELO_LIST=${ELO_LIST:-"0.10 0.20 0.30"}
THR_LIST=${THR_LIST:-"0.34 0.38 0.40 0.42 0.46"}

BASE_URL="http://127.0.0.1:3001"
OUT="cache/logs/env_sweep.tsv"

log(){ printf '%s\n' "[env-sweep] $*"; }

cd /opt/edge-trader-server

# Старт отдельного env-сервера на :3001
log "starting env server on :3001"
ETAI_ENABLE_TRAIN_ENV=1 ./build/edge_trader_server 3001 >/tmp/env.log 2>&1 & echo $! >/tmp/env.pid

# Ожидание готовности
for i in $(seq 1 50); do
  if curl -sS "${BASE_URL}/api/health" >/dev/null 2>&1; then
    log "env server is up"
    break
  fi
  sleep 0.1
done
sleep 0.3

# Заголовок TSV
printf "AGGR_K\tE_LO\tTHR\tSTEPS\tWINRATE\tPF\tSHARPE\tMAX_DD\tEQUITY_FINAL\n" > "$OUT"

# Грид-свип
for AGGR in $AGGR_LIST; do
  for ELO in $ELO_LIST; do
    for THR in $THR_LIST; do
      URL="${BASE_URL}/api/train_env?steps=${STEPS}&policy=model&fee=${FEE}&tp=${TP}&sl=${SL}&thr=${THR}"
      # Мягкий контекст-гейт
      ETAI_FEAT_ENABLE_MFLOW=1 \
      ETAI_CTX_GATE_MODE=soft \
      ETAI_CTX_E_LO="${ELO}" \
      ETAI_AGGR_K="${AGGR}" \
      curl -sS "$URL" \
      | jq -r --arg ag "$AGGR" --arg el "$ELO" --arg th "$THR" '
          [
            ($ag|tonumber),
            ($el|tonumber),
            ($th|tonumber),
            (.steps // 0),
            (.winrate // 0),
            (.pf // 0),
            (.sharpe // 0),
            (.max_dd // 0),
            (.equity_final // 0)
          ] | @tsv
        ' >> "$OUT"
      printf '.'
    done
  done
done
printf '\n'

# Стоп env
if [[ -f /tmp/env.pid ]]; then
  kill "$(cat /tmp/env.pid)" >/dev/null 2>&1 || true
  rm -f /tmp/env.pid
fi

log "results saved to $OUT"

# Топ-10 по winrate (desc), потом по PF, потом по Sharpe
log "TOP 10 by WINRATE:"
awk 'NR==1{print; next} {print | "sort -t\"\t\" -k5,5nr -k6,6nr -k7,7nr | head -n 10"}' "$OUT"
