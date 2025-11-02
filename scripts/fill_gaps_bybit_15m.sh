#!/usr/bin/env bash
set -euo pipefail

SYM="${1:?usage: fill_gaps_bybit_15m.sh <SYMBOL>}"
CACHE="/opt/edge-trader-server/cache"
RAW="$CACHE/${SYM}_15.csv"
CLEAN="$CACHE/clean/${SYM}_15.csv"

command -v curl >/dev/null || { echo "curl not found" >&2; exit 3; }
command -v jq   >/dev/null || { echo "jq not found" >&2; exit 3; }
[ -f "$RAW" ]   || { echo "RAW not found: $RAW" >&2; exit 2; }
[ -f "$CLEAN" ] || { echo "CLEAN not found: $CLEAN" >&2; exit 2; }

# 1) Найти пропуски по CLEAN (шаг 900000 мс)
mapfile -t GAPS < <(awk -F, '
  NR==1{prev=$1; next}
  {
    while ($1-prev>900000) { prev+=900000; print prev }
    prev=$1
  }' "$CLEAN")

GAPS_CNT="${#GAPS[@]}"
echo "Detected GAPS_15m=$GAPS_CNT for $SYM"
[ "$GAPS_CNT" -gt 0 ] || { echo "No gaps. Nothing to do."; exit 0; }

# 2) Функция вытянуть одну свечу с Bybit v5 (spot, interval=15)
fetch_one() {
  local ts="$1"
  local start="$ts"
  local end="$((ts+899999))"
  # Bybit v5: /v5/market/kline?category=spot&symbol=OPUSDT&interval=15&start=...&end=...&limit=1
  local url="https://api.bybit.com/v5/market/kline?category=spot&symbol=${SYM}&interval=15&start=${start}&end=${end}&limit=1"
  # Формат v5: result.list = [ [start, open, high, low, close, volume, turnover], ... ]
  curl -fsS "$url" \
    | jq -r '
        .result.list
        | if type=="array" and length>0 then .[0] else empty end
        | @csv' \
    | sed 's/"//g'
}

TMP_RAW_ADDS="$(mktemp)"
trap 'rm -f "$TMP_RAW_ADDS"' EXIT

echo "Fetching missing candles from Bybit..."
ADDED=0
for ts in "${GAPS[@]}"; do
  line="$(fetch_one "$ts" || true)"
  if [ -n "$line" ]; then
    # Ожидаем ровно 7 полей (ts,open,high,low,close,volume,turnover)
    if [ "$(awk -F, '{print NF}' <<<"$line")" -eq 7 ]; then
      echo "$line" >> "$TMP_RAW_ADDS"
      ADDED=$((ADDED+1))
    else
      echo "WARN: Bad field count for ts=$ts: $line" >&2
    fi
  else
    echo "WARN: No data returned for ts=$ts" >&2
  fi
done

echo "Added (fetched) rows: $ADDED"

if [ "$ADDED" -eq 0 ]; then
  echo "Nothing added. Exit."
  exit 0
fi

# 3) Слить в RAW с сортировкой и удалением дублей по ts
# Обеспечиваем уникальность по первому столбцу, сортируем по ts
awk -F, 'FNR==NR{a[$1]=$0; next} {a[$1]=$0} END{for(k in a) print a[k]}' "$RAW" "$TMP_RAW_ADDS" \
  | awk -F, 'NF==7' \
  | sort -t, -k1,1n \
  > "${RAW}.new"

mv "${RAW}.new" "$RAW"
echo "RAW merged: $(wc -l < "$RAW") rows"

# 4) Пересобрать CLEAN из RAW вашим нормализатором
if [ -x "scripts/clean_from_raw.sh" ]; then
  bash scripts/clean_from_raw.sh "$SYM"
else
  echo "ERROR: scripts/clean_from_raw.sh not found or not executable" >&2
  exit 4
fi

# 5) Повторная проверка шага 15m
if awk -F, 'NR>1 {d=$1-prev; if(d!=900000) bad++; prev=$1} NR==1 {prev=$1} END{exit (bad?1:0)}' "$CLEAN"; then
  echo "[OK] 15m deltas=900000 ms без дыр — $CLEAN"
else
  echo "[FAIL] остались пропуски — $CLEAN" >&2
  exit 5
fi

# 6) Краткая сводка диапазона
TS_MIN="$(head -1 "$CLEAN" | cut -d, -f1)"
TS_MAX="$(tail -1 "$CLEAN" | cut -d, -f1)"
EXP=$(( (TS_MAX-TS_MIN)/900000 + 1 ))
ACT="$(wc -l < "$CLEAN")"
printf "SUMMARY: EXPECTED=%d ACTUAL=%d COVERAGE=%.4f\n" "$EXP" "$ACT" "$(awk -v e=$EXP -v a=$ACT 'BEGIN{printf("%.4f", a/e)}')"

echo "Done."
