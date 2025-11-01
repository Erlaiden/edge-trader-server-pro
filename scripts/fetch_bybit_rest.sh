#!/usr/bin/env bash
set -euo pipefail

: "${BYBIT_API:=https://api.bybit.com}"
CATEGORY="${CATEGORY:-linear}"
SYMBOL="${SYMBOL:-BTCUSDT}"
MONTHS="${MONTHS:-6}"
INTERVALS="${INTERVALS:-"15 60 240 1440"}"
OUTDIR="${OUTDIR:-cache}"
VERBOSE="${VERBOSE:-0}"   # 1 — печатать прогресс по каждому батчу

JQ_BIN="${JQ_BIN:-jq}"
CURL_OPTS=( --fail --show-error --silent --retry 3 --retry-delay 1 -H "Accept: application/json" )
die(){ echo "[FAIL] $*" >&2; exit 1; } ; need(){ command -v "$1" >/dev/null || die "missing tool: $1"; }
need curl; need "$JQ_BIN"; mkdir -p "$OUTDIR"

now_ms() { date -u +%s000; }
months_ago_ms() { python3 - "$MONTHS" <<'PY'
import sys, datetime
m=int(sys.argv[1])
now=datetime.datetime.now(datetime.UTC)
start=now-datetime.timedelta(days=30*m)
print(int(start.timestamp()*1000))
PY
}
map_interval(){ case "$1" in 1|3|5|15|30|60|120|240|360|720) echo "$1";;
  1440|d|D|day|DAY) echo "D";; 10080|w|W|week|WEEK) echo "W";;
  43200|m|M|month|MONTH) echo "M";; *) die "unsupported interval: $1";; esac; }

echo "[INFO] validate symbol: category=$CATEGORY symbol=$SYMBOL"
resp="$(curl "${CURL_OPTS[@]}" "${BYBIT_API}/v5/market/instruments-info?category=${CATEGORY}&symbol=${SYMBOL}" || true)"
[ "$(echo "$resp" | $JQ_BIN -r '.retCode')" = "0" ] || die "instruments-info error: $(echo "$resp" | $JQ_BIN -c '.retMsg,.retCode')"
[ "$(echo "$resp" | $JQ_BIN -r '.result.list | length')" -ge 1 ] || die "symbol not found: $SYMBOL"
echo "[OK] symbol exists"

END_MS="$(now_ms)"; START_MS="$(months_ago_ms)"
[ "$START_MS" -lt "$END_MS" ] || die "start >= end"

for tf in $INTERVALS; do
  BI="$(map_interval "$tf")"
  outfile="${OUTDIR}/${SYMBOL}_$([ "$BI" = "D" ] && echo 1440 || echo "$BI").csv"
  tmpfile="$(mktemp)"; : > "$tmpfile"
  case "$BI" in D) bar_ms=$((24*60*60*1000));; W) bar_ms=$((7*24*60*60*1000));;
                 M) bar_ms=$((30*24*60*60*1000));; *) bar_ms=$((BI*60*1000));; esac
  echo "[INFO] backfill: tf=${tf} (bybit=${BI}) -> ${outfile}"
  start="$START_MS"; total=0; batches=0
  while [ "$start" -lt "$END_MS" ]; do
    url="${BYBIT_API}/v5/market/kline?category=${CATEGORY}&symbol=${SYMBOL}&interval=${BI}&start=${start}&end=${END_MS}&limit=1000"
    json="$(curl "${CURL_OPTS[@]}" "$url" || true)"
    rc="$(echo "$json" | $JQ_BIN -r '.retCode // empty')"
    [ "$rc" = "0" ] || { echo "[WARN] kline retCode=$rc msg=$(echo "$json"|$JQ_BIN -r '.retMsg')" >&2; break; }
    rows="$(echo "$json" | $JQ_BIN -r '.result.list | length')" ; [ "$rows" = "null" ] && rows=0
    [ "$rows" -eq 0 ] && { [ "$VERBOSE" = "1" ] && echo "[VERB] empty batch"; break; }
    echo "$json" | $JQ_BIN -r '.result.list[] | @csv' \
      | awk -F',' '{gsub(/"/,"",$0); printf "%s,%s,%s,%s,%s,%s\n",$1,$2,$3,$4,$5,$6}' >> "$tmpfile"
    last_open_ms="$(echo "$json" | $JQ_BIN -r '.result.list[-1][0]')"
    start=$(( last_open_ms + bar_ms ))
    total=$((total+rows)); batches=$((batches+1))
    if [ "$VERBOSE" = "1" ]; then
      echo "[VERB] batch=${batches} rows=${rows} total=${total} next_from=$(date -u -d @$(($start/1000)) +%F' '%T)Z"
    fi
    sleep 0.05
  done
  if [ -s "$tmpfile" ]; then
    sort -t, -k1,1n "$tmpfile" | awk -F',' '!seen[$1]++' > "${outfile}.tmp" && mv -f "${outfile}.tmp" "$outfile"
    echo "[OK] ${outfile}: rows=$(wc -l < "$outfile")"
  else
    echo "[WARN] no data for tf=${tf}"
    rm -f "$tmpfile"
  fi
done
echo "[DONE] Backfill complete for ${SYMBOL} (${MONTHS} mo) in: ${OUTDIR}"
