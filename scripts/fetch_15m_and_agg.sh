#!/usr/bin/env bash
set -euo pipefail

SYM="${1:?usage: fetch_15m_and_agg.sh <SYMBOL> [months]}"
MONTHS="${2:-12}"

CACHE="/opt/edge-trader-server/cache"
CLEAN="$CACHE/clean"
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

# Сколько баров 15m хотим (≈ 96 в сутки; 30 дней -> 2880)
let "TARGET = MONTHS * 30 * 96"
if (( TARGET < 2000 )); then TARGET=2000; fi

RAW15="$CACHE/${SYM}_15.csv"
RAW60="$CACHE/${SYM}_60.csv"
RAW240="$CACHE/${SYM}_240.csv"
RAW1440="$CACHE/${SYM}_1440.csv"

CLN15="$CLEAN/${SYM}_15.csv"
CLN60="$CLEAN/${SYM}_60.csv"
CLN240="$CLEAN/${SYM}_240.csv"
CLN1440="$CLEAN/${SYM}_1440.csv"

# --- 1) тянем 15m пачками по 1000 с cursor ---
API="https://api.bybit.com/v5/market/kline"
Q="category=linear&symbol=${SYM}&interval=15&limit=1000"
CURSOR=""
MERGED="$TMPDIR/merged15.jsonl"

echo "Downloading ${SYM} 15m ~${TARGET} bars from Bybit..." >&2
COUNT=0
ITER=0

while :; do
  URL="${API}?${Q}"
  if [[ -n "$CURSOR" ]]; then URL="${URL}&cursor=${CURSOR}"; fi
  RESP="$TMPDIR/r${ITER}.json"
  curl -sS "$URL" -o "$RESP"

  RET="$(jq -r '.retCode' "$RESP" 2>/dev/null || echo 1)"
  if [[ "$RET" != "0" ]]; then
    echo "Bybit error retCode=${RET} $(jq -r '.retMsg//empty' "$RESP")" >&2
    exit 1
  fi

  # list — массив строк [start,open,high,low,close,volume,turnover]
  N="$(jq -r '.result.list | length' "$RESP")"
  if [[ "$N" == "0" || "$N" == "null" ]]; then
    break
  fi

  jq -r '.result.list[] | @tsv' "$RESP" >> "$MERGED"
  COUNT=$(( COUNT + N ))

  # следующий курсор
  CURSOR="$(jq -r '.result.nextPageCursor // empty' "$RESP")"
  ITER=$((ITER+1))

  if (( COUNT >= TARGET )); then
    break
  fi
  if [[ -z "$CURSOR" ]]; then
    break
  fi
done

if [[ ! -s "$MERGED" ]]; then
  echo "No data downloaded for ${SYM}" >&2
  exit 2
fi

# --- 2) конвертируем в CSV RAW 15m: ts,open,high,low,close,volume,turnover ---
# вход в jsonl: start,open,high,low,close,volume,turnover (все строки)
# сортируем по ts и убираем дубликаты ts
{
  echo "ts,open,high,low,close,volume,turnover"
  awk -v OFS=',' '
    {
      ts=$1; open=$2; high=$3; low=$4; close=$5; vol=$6; turn=$7;
      # вход из jq @tsv — поля таб-разделены строками
    } BEGIN{FS="\t"; OFS=","}
  ' "$MERGED" | sed '1d' | tr '\t' ',' \
  | awk -F',' 'NR==1{print "ts,open,high,low,close,volume,turnover"; next} {print}' \
  > "$TMPDIR/_raw15.csv"
} 2>/dev/null || true

# Из-за заголовка выше перезапишем более аккуратно:
printf "ts,open,high,low,close,volume,turnover\n" > "$RAW15"
tail -n +2 "$TMPDIR/_raw15.csv" \
 | sort -t, -k1,1n \
 | awk -F, '!seen[$1]++' OFS=',' >> "$RAW15"

R15=$(($(wc -l < "$RAW15")-1))
echo "Wrote $RAW15 ($R15 rows)" >&2

# --- 3) CLEAN 15m: 6 колонок без turnover ---
printf "ts,open,high,low,close,volume\n" > "$CLN15"
tail -n +2 "$RAW15" | awk -F, -v OFS=',' '{print $1,$2,$3,$4,$5,$6}' >> "$CLN15"
echo "Wrote $CLN15 ($(($(wc -l < "$CLN15")-1)) rows)" >&2

# --- 4) агрегатор из 15m → TF (60/240/1440) ---
agg_from_15m() {
  local IN15="$1" TF="$2" OUTRAW="$3" OUTCLN="$4"
  local GROUP=$(( TF / 15 ))
  if (( GROUP < 1 )); then GROUP=1; fi

  printf "ts,open,high,low,close,volume,turnover\n" > "$OUTRAW"
  tail -n +2 "$IN15" \
  | awk -F, -v OFS=',' -v G="$GROUP" '
      {
        # вычисляем бакет по количеству баров (не по календарю — нам синтетика для фичей)
        idx = NR-1; bucket = int(idx / G);
        if (bucket != prev && NR>1) {
          print ts, o, h, l, c, v, 0;
          v=0;
        }
        if (bucket != prev) {
          ts=$1; o=$2; h=$3; l=$4; c=$5; v=$6;
          prev=bucket;
        } else {
          if ($3>h) h=$3;
          if ($4<l) l=$4;
          c=$5;
          v+=($6+0);
        }
      }
      END { if (NR>0) print ts,o,h,l,c,v,0; }
    ' >> "$OUTRAW"

  printf "ts,open,high,low,close,volume\n" > "$OUTCLN"
  tail -n +2 "$OUTRAW" | awk -F, -v OFS=',' '{print $1,$2,$3,$4,$5,$6}' >> "$OUTCLN"
}

agg_from_15m "$RAW15" 60   "$RAW60"   "$CLN60"
echo "Built $RAW60 ($(($(wc -l < "$RAW60")-1)) rows)"
agg_from_15m "$RAW15" 240  "$RAW240"  "$CLN240"
echo "Built $RAW240 ($(($(wc -l < "$RAW240")-1)) rows)"
agg_from_15m "$RAW15" 1440 "$RAW1440" "$CLN1440"
echo "Built $RAW1440 ($(($(wc -l < "$RAW1440")-1)) rows)"

# Финал
exit 0
