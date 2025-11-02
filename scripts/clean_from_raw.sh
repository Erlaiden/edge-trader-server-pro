#!/usr/bin/env bash
set -euo pipefail

SYM="${1:?usage: clean_from_raw.sh <SYMBOL>}"
CACHE="/opt/edge-trader-server/cache"
CLEAN="$CACHE/clean"
mkdir -p "$CLEAN"

TFS=("15" "60" "240" "1440")

for TF in "${TFS[@]}"; do
  RAW="$CACHE/${SYM}_${TF}.csv"
  OUT="$CLEAN/${SYM}_${TF}.csv"

  if [[ ! -s "$RAW" ]]; then
    echo "[skip] $RAW not found or empty" >&2
    continue
  fi

  # фильтрация, сортировка, удаление дублей
  awk -F',' 'NF==7 && $1 ~ /^[0-9]+$/ {print $0}' "$RAW" \
    | sort -t, -k1,1n \
    | awk -F',' -v OFS=',' '!seen[$1]++ {print $1,$2,$3,$4,$5,$6}' > "$OUT"

  ROWS=$(wc -l < "$OUT" | tr -d ' ')
  echo "[ok]  $OUT (rows=$ROWS)"
done

echo "=== CLEAN HEADS ==="
for TF in "${TFS[@]}"; do
  OUT="$CLEAN/${SYM}_${TF}.csv"
  if [[ -s "$OUT" ]]; then
    echo "-- $OUT  (rows=$(wc -l < "$OUT"))"
    head -n 3 "$OUT"
  fi
done
