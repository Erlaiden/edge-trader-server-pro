#!/usr/bin/env bash
set -euo pipefail

SYM="${1:?usage: fetch_15m_and_agg.sh <SYMBOL> [months]}"
MONTHS="${2:-12}"

CACHE="/opt/edge-trader-server/cache"
CLEAN="$CACHE/clean"

# Инструменты
command -v curl >/dev/null || { echo "curl not found" >&2; exit 3; }
command -v jq   >/dev/null || { echo "jq not found" >&2; exit 3; }

mkdir -p "$CACHE" "$CLEAN"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

# Сколько баров 15m хотим (≈96/сутки; 30 дней ≈ 2880)
TARGET=$(( MONTHS * 30 * 96 ))
(( TARGET < 2000 )) && TARGET=2000

RAW15="$CACHE/${SYM}_15.csv"
RAW60="$CACHE/${SYM}_60.csv"
RAW240="$CACHE/${SYM}_240.csv"
RAW1440="$CACHE/${SYM}_1440.csv"

CLN15="$CLEAN/${SYM}_15.csv"
CLN60="$CLEAN/${SYM}_60.csv"
CLN240="$CLEAN/${SYM}_240.csv"
CLN1440="$CLEAN/${SYM}_1440.csv"

# --- 1) тянем 15m по курсору Bybit v5 ---
API="https://api.bybit.com/v5/market/kline"
Q="category=linear&symbol=${SYM}&interval=15&limit=1000"
CURSOR=""
MERGED_TSV="$TMPDIR/merged15.tsv"

echo "Downloading ${SYM} 15m ~${TARGET} bars from Bybit..." >&2
COUNT=0
ITER=0

while :; do
  URL="${API}?${Q}"
  [[ -n "$CURSOR" ]] && URL="${URL}&cursor=${CURSOR}"

  RESP="$TMPDIR/r${ITER}.json"
  curl -sS "$URL" -o "$RESP"

  RET="$(jq -r '.retCode' "$RESP" 2>/dev/null || echo 1)"
  if [[ "$RET" != "0" ]]; then
    echo "Bybit error retCode=${RET} $(jq -r '.retMsg//empty' "$RESP")" >&2
    exit 1
  fi

  N="$(jq -r '.result.list | length' "$RESP")"
  [[ "$N" == "0" || "$N" == "null" ]] && break

  # list: [start,open,high,low,close,volume,turnover] — строки
  jq -r '.result.list[] | @tsv' "$RESP" >> "$MERGED_TSV"
  COUNT=$(( COUNT + N ))

  CURSOR="$(jq -r '.result.nextPageCursor // empty' "$RESP")"
  ITER=$((ITER+1))

  (( COUNT >= TARGET )) && break
  [[ -z "$CURSOR" ]] && break
done

# Проверка загрузки
if [[ ! -s "$MERGED_TSV" ]]; then
  echo "No data downloaded for ${SYM}" >&2
  exit 2
fi

# --- 2) RAW 15m: 7 колонок БЕЗ заголовка: ts,open,high,low,close,volume,turnover ---
# Сортируем по ts и убираем дубли
awk -F'\t' 'NF>=7 {print $1","$2","$3","$4","$5","$6","$7}' "$MERGED_TSV" \
  | sort -t, -k1,1n \
  | awk -F, '!seen[$1]++' \
  > "$TMPDIR/_raw15.csv"

# Фильтр на валидные числовые поля (ts и цены/объём)
awk -F, 'NF==7 && $1 ~ /^[0-9]+$/ && $2+0==$2 && $3+0==$3 && $4+0==$4 && $5+0==$5 && $6+0==$6 && $7+0==$7' \
  "$TMPDIR/_raw15.csv" > "$RAW15"

R15=$(wc -l < "$RAW15")
echo "Wrote $RAW15 ($R15 rows)" >&2
if (( R15 == 0 )); then
  echo "Empty RAW15 after filtering — abort" >&2
  exit 2
fi

# --- 3) CLEAN 15m: 6 колонок (ts,open,high,low,close,volume), БЕЗ заголовка ---
awk -F, 'NF==7 {print $1","$2","$3","$4","$5","$6}' "$RAW15" > "$CLN15"
echo "Wrote $CLN15 ($(wc -l < "$CLN15") rows)" >&2

# --- 4) агрегатор из 15m -> (60/240/1440) по фиксированному размеру корзины (не календарю) ---
agg_from_15m() {
  local IN15="$1" TF="$2" OUTRAW="$3" OUTCLN="$4"
  local GROUP=$(( TF / 15 ))
  (( GROUP < 1 )) && GROUP=1

  # читаем без заголовка: ts,open,high,low,close,volume,turnover
  awk -F, -v OFS=',' -v G="$GROUP" '
    NR==1 {
      ts=$1; o=$2; h=$3; l=$4; c=$5; v=$6; n=1; next
    }
    {
      idx = n; bucket = int(idx / G);
      if (bucket != prev && n>0) {
        print ts,o,h,l,c,v,0
        v=0
      }
      if (bucket != prev) {
        ts=$1; o=$2; h=$3; l=$4; c=$5; v=$6; prev=bucket
      } else {
        if ($3>h) h=$3
        if ($4<l) l=$4
        c=$5
        v+=($6+0)
      }
      n++
    }
    END { if (n>0) print ts,o,h,l,c,v,0 }
  ' "$IN15" > "$OUTRAW"

  # CLEAN: 6 колонок, без заголовка
  awk -F, -v OFS=',' '{print $1,$2,$3,$4,$5,$6}' "$OUTRAW" > "$OUTCLN"
}

agg_from_15m "$RAW15" 60   "$RAW60"   "$CLN60"
echo "Built $RAW60 ($(wc -l < "$RAW60") rows)" >&2
agg_from_15m "$RAW15" 240  "$RAW240"  "$CLN240"
echo "Built $RAW240 ($(wc -l < "$RAW240") rows)" >&2
agg_from_15m "$RAW15" 1440 "$RAW1440" "$CLN1440"
echo "Built $RAW1440 ($(wc -l < "$RAW1440") rows)" >&2

exit 0
