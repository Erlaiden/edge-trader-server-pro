#!/usr/bin/env bash
set -euo pipefail

TMP_REQ=$(mktemp)
TMP_RESP=$(mktemp)
trap 'rm -f "$TMP_REQ" "$TMP_RESP"' EXIT

cat > "$TMP_REQ"

# Разбор JSON из тела POST
SYM=$(jq -r '.symbol' "$TMP_REQ")
MONTHS=$(jq -r '.months // 6' "$TMP_REQ")
TP=$(jq -r '.tp // 0.006' "$TMP_REQ")
SL=$(jq -r '.sl // 0.0024' "$TMP_REQ")
MA=$(jq -r '.ma // 12' "$TMP_REQ")

# Проверка аргументов
if [[ -z "$SYM" || "$SYM" == "null" ]]; then
  echo "HTTP/1.1 400 Bad Request"
  echo "Content-Type: application/json"
  echo
  echo '{"ok":false,"error":"symbol_required"}'
  exit 0
fi

LOCK="/tmp/edge_pipeline_${SYM}.lock"
exec 9>"$LOCK"
if ! flock -n 9; then
  echo "HTTP/1.1 423 Locked"
  echo "Content-Type: application/json"
  echo
  echo "{\"ok\":false,\"error\":\"pipeline_running\",\"symbol\":\"${SYM}\"}"
  exit 0
fi

LOGFILE="/tmp/pipeline_${SYM}_$(date -u +'%Y%m%d-%H%M%SZ').log"
echo "{\"ok\":true,\"stage\":\"running\",\"symbol\":\"${SYM}\"}" > "$TMP_RESP"

# === Основной вызов пайплайна ===
/opt/edge-trader-server/scripts/prepare_and_train_15m.sh "$SYM" "$MONTHS" "$TP" "$SL" "$MA" >"$LOGFILE" 2>&1 || {
  echo "HTTP/1.1 500 Internal Server Error"
  echo "Content-Type: application/json"
  echo
  jq -n --arg sym "$SYM" --arg stage "train_failed" --arg log "$LOGFILE" \
     '{ok:false, symbol:$sym, error:$stage, log:$log}'
  exit 0
}

# Формируем результат
MODEL_PATH=$(grep -m1 'MODEL:' "$LOGFILE" | awk '{print $2}')
BEST_THR=$(grep -m1 'BEST_THR:' "$LOGFILE" | awk '{print $2}')

echo "HTTP/1.1 200 OK"
echo "Content-Type: application/json"
echo
jq -n --arg sym "$SYM" --arg model "$MODEL_PATH" --arg thr "$BEST_THR" --arg log "$LOGFILE" \
  '{ok:true, symbol:$sym, model:$model, best_thr:$thr, log:$log}'
