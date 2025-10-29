#!/usr/bin/env bash
set -euo pipefail

BASE_URL="http://127.0.0.1:3000"
LOGDIR="/opt/edge-trader-server/cache/logs"
LOCKFILE="/opt/edge-trader-server/cache/train.lock"
mkdir -p "$LOGDIR"

TS="$(date -u +%Y%m%d_%H%M%S)"
JSON_OUT="$LOGDIR/train_${TS}.json"
LOGFILE="$LOGDIR/train.timer.log"

# простая ротация лога до ~1MB
if [ -f "$LOGFILE" ] && [ "$(wc -c < "$LOGFILE")" -gt 1048576 ]; then
  mv -f "$LOGFILE" "$LOGFILE.1" || true
fi

# эксклюзивный запуск (не даём таймеру наложиться)
exec 9>"$LOCKFILE"
if ! flock -n 9; then
  echo "[TRAIN][${TS}] skip: already running" | tee -a "$LOGFILE"
  exit 0
fi

echo "[TRAIN][${TS}] start episodes=60 tp=0.008 sl=0.0032 ma=12" | tee -a "$LOGFILE"

# 1) Тренировка (таймауты + fail с телом)
if ! curl --fail-with-body --max-time 120 --retry 2 --retry-delay 2 \
  -sS "${BASE_URL}/api/train?symbol=BTCUSDT&interval=15&episodes=60&tp=0.008&sl=0.0032&ma=12" \
  | tee "$JSON_OUT" >/dev/null ; then
  echo "[TRAIN][${TS}] ERROR: train request failed" | tee -a "$LOGFILE"
  exit 2
fi

# 2) Принудительно прогружаем модель в память (best-effort)
curl -sS --max-time 30 "${BASE_URL}/api/model?symbol=BTCUSDT&interval=15" >/dev/null || true

# 3) Сводка в лог
OK="false"; PATHJSON=""; OOS_SH="0"; THR="0"; MA="0"
if command -v jq >/dev/null 2>&1; then
  OK=$(jq -r '.ok // false' "$JSON_OUT" 2>/dev/null || echo false)
  PATHJSON=$(jq -r '.model_path // ""' "$JSON_OUT" 2>/dev/null || echo "")
  OOS_SH=$(jq -r '.metrics.oos_summary.sharpe // 0' "$JSON_OUT" 2>/dev/null || echo 0)
  THR=$(jq -r '.metrics.best_thr // 0' "$JSON_OUT" 2>/dev/null || echo 0)
  MA=$(jq -r '.metrics.ma_len // 0' "$JSON_OUT" 2>/dev/null || echo 0)
fi
echo "[TRAIN][${TS}] ok=${OK} path=${PATHJSON} oos_sh=${OOS_SH} thr=${THR} ma=${MA}" | tee -a "$LOGFILE"

# 4) Короткий пост-инференс (для контроля used_policy)
POST="$(curl -sS --max-time 30 "${BASE_URL}/api/infer?symbol=BTCUSDT&interval=15" \
  | jq -r '{ok,used_policy,signal,score} | @json' 2>/dev/null || echo '{}')"
echo "[TRAIN][${TS}] infer=${POST}" | tee -a "$LOGFILE"

echo "[TRAIN][${TS}] done" | tee -a "$LOGFILE"
