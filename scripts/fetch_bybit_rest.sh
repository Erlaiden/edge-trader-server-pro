#!/usr/bin/env bash
set -euo pipefail

# ===== Config / Inputs =====
: "${BYBIT_API:=https://api.bybit.com}"
CATEGORY="${CATEGORY:-linear}"          # linear | spot | inverse | option (обычно linear для USDT perp)
SYMBOL="${SYMBOL:-BTCUSDT}"             # пример: BTCUSDT
MONTHS="${MONTHS:-6}"                   # сколько месяцев назад тянем
INTERVALS="${INTERVALS:-"15 60 240 1440"}"  # 1440 будет замаплен в D
OUTDIR="${OUTDIR:-cache}"

JQ_BIN="${JQ_BIN:-jq}"
CURL_OPTS=(
  --fail --show-error --silent
  --retry 3 --retry-delay 1
  -H "Accept: application/json"
)

# ===== Helpers =====
die(){ echo "[FAIL] $*" >&2; exit 1; }
need(){ command -v "$1" >/dev/null || die "missing tool: $1"; }

need curl
need "$JQ_BIN"
mkdir -p "$OUTDIR"

# ms since epoch (UTC)
now_ms() { date -u +%s000; }

# Примерная нижняя граница на MONTHS назад (UTC); точность до суток нас устраивает
months_ago_ms() {
  python3 - "$MONTHS" <<'PY'
import sys, datetime, calendar
m=int(sys.argv[1])
now=datetime.datetime.utcnow()
# грубо: m*30 суток назад
delta=datetime.timedelta(days=30*m)
start=now-delta
ms=int(start.timestamp()*1000)
print(ms)
PY
}

# Маппинг интервалов из наших чисел в bybit v5
map_interval(){
  local tf="$1"
  case "$tf" in
    1|3|5|15|30|60|120|240|360|720) echo "$tf" ;;
    1440|d|D|day|DAY) echo "D" ;;
    10080|w|W|week|WEEK) echo "W" ;;
    43200|m|M|month|MONTH) echo "M" ;;
    *) die "unsupported interval: $tf"
  esac
}

# ===== 0) Валидация символа =====
echo "[INFO] validate symbol via instruments-info: category=$CATEGORY symbol=$SYMBOL"
inst_url="${BYBIT_API}/v5/market/instruments-info?category=${CATEGORY}&symbol=${SYMBOL}"
resp="$(curl "${CURL_OPTS[@]}" "$inst_url" || true)"
retCode="$(echo "$resp" | $JQ_BIN -r '.retCode // empty')"
[ -z "$retCode" ] && die "empty response from instruments-info"
[ "$retCode" != "0" ] && die "instruments-info error: $(echo "$resp" | $JQ_BIN -c '.retMsg, .retCode')"
cnt="$(echo "$resp" | $JQ_BIN -r '.result.list | length')"
[ "$cnt" -lt 1 ] && die "symbol not found in this category: $SYMBOL ($CATEGORY)"
echo "[OK] symbol exists"

# ===== 1) Границы времени =====
END_MS="$(now_ms)"
START_MS="$(months_ago_ms)"
[ "$START_MS" -ge "$END_MS" ] && die "start >= end (check system clock)"

# ===== 2) По каждому интервалу — бэкафилл батчами по 1000 =====
for tf in $INTERVALS; do
  BI="$(map_interval "$tf")"  # Bybit interval
  outfile="${OUTDIR}/${SYMBOL}_$([ "$BI" = "D" ] && echo 1440 || echo "$BI").csv"
  tmpfile="$(mktemp)"
  : > "$tmpfile"

  echo "[INFO] backfill: tf=$tf (bybit=$BI) -> $outfile"
  start="$START_MS"
  iter=0
  total=0

  # Подсказка по ms длине бара
  case "$BI" in
    D)  bar_ms=$((24*60*60*1000));;
    W)  bar_ms=$((7*24*60*60*1000));;
    M)  bar_ms=$((30*24*60*60*1000));; # грубо
    *)  bar_ms=$((BI*60*1000));;
  esac

  while [ "$start" -lt "$END_MS" ]; do
    end=$(( start + bar_ms*1000 )) # широкий верх, Bybit сам отсечёт по limit
    url="${BYBIT_API}/v5/market/kline?category=${CATEGORY}&symbol=${SYMBOL}&interval=${BI}&start=${start}&end=${END_MS}&limit=1000"
    json="$(curl "${CURL_OPTS[@]}" "$url" || true)"
    rc="$(echo "$json" | $JQ_BIN -r '.retCode // empty')"
    [ -z "$rc" ] && die "empty response from kline"
    if [ "$rc" != "0" ]; then
      echo "[WARN] kline retCode=$rc retMsg=$(echo "$json"|$JQ_BIN -r '.retMsg')" >&2
      break
    fi

    # result.list — массив строк: [ openTime, open, high, low, close, volume, turnover ]
    rows="$(echo "$json" | $JQ_BIN -r '.result.list | length')"
    [ "$rows" = "null" ] && rows=0

    if [ "$rows" -eq 0 ]; then
      # нечего добавлять — выходим из цикла данного ТФ
      break
    fi

    # Записываем в tmp построчно как CSV epoch_ms,open,high,low,close,volume
    echo "$json" | $JQ_BIN -r '.result.list[] | @csv' \
      | awk -F',' '{
          # поля: 1=openTime,2=open,3=high,4=low,5=close,6=volume,7=turnover
          gsub(/"/,"",$1); gsub(/"/,"",$2); gsub(/"/,"",$3); gsub(/"/,"",$4); gsub(/"/,"",$5); gsub(/"/,"",$6);
          printf "%s,%s,%s,%s,%s,%s\n", $1,$2,$3,$4,$5,$6
        }' >> "$tmpfile"

    # Обновляем start: + rows*bar_ms (идём вперёд)
    last_open_ms="$(echo "$json" | $JQ_BIN -r '.result.list[-1][0]')"
    if [ -z "$last_open_ms" ] || [ "$last_open_ms" = "null" ]; then
      break
    fi
    start=$(( last_open_ms + bar_ms ))
    iter=$((iter+1))
    total=$((total+rows))
    # чуть бережно к лимитам
    sleep 0.05
  done

  # Сортируем и удаляем дубликаты по ts
  if [ -s "$tmpfile" ]; then
    sort -t, -k1,1n "$tmpfile" | awk -F',' '!seen[$1]++' > "${outfile}.tmp"
    mv -f "${outfile}.tmp" "$outfile"
    echo "[OK] ${outfile}: rows=$(wc -l < "$outfile")"
  else
    echo "[WARN] no data for tf=$tf"
    rm -f "$tmpfile"
  fi
done

# Итог
echo "[DONE] Backfill complete for ${SYMBOL} (${MONTHS} mo) in: ${OUTDIR}"
