#!/usr/bin/env bash
set -euo pipefail

# ===== INPUT =====
CATEGORY="${CATEGORY:-linear}"                # linear | inverse | spot
SYMBOL="${SYMBOL:-BTCUSDT}"
MONTHS="${MONTHS:-6}"                         # последние ~30*MONTHS дней
INTERVALS="${INTERVALS:-"15 60 240 1440"}"    # 240 и 1440 считаем из 60
VERBOSE="${VERBOSE:-1}"
API="https://api.bybit.com"

log(){ [ "${VERBOSE}" = "1" ] && echo "[$(date -u +%H:%M:%S)] $*"; }
need(){ command -v "$1" >/dev/null 2>&1 || { echo "[FAIL] need $1"; exit 1; }; }
need curl; need jq; need awk; need sort
mkdir -p cache cache/clean tmp

# ---- map tf -> bybit interval token ----
map_bybit_tf(){
  case "$1" in
    1|3|5|15|30|60|120|240|360|720) echo "$1" ;;
    1440) echo "D" ;;
    *) echo "__BAD__" ;;
  esac
}

# ---- symbol validation ----
log "validate symbol via instruments-info: category=${CATEGORY} symbol=${SYMBOL}"
curl -sS "${API}/v5/market/instruments-info?category=${CATEGORY}&symbol=${SYMBOL}" \
| jq -e '.result.list|length>0' >/dev/null \
  || { echo "[FAIL] unknown symbol for this category"; exit 1; }
log "[OK] symbol exists"

# ---- time window ----
NOW_MS=$(python3 - <<'PY'
from datetime import datetime, timezone
print(int(datetime.now(timezone.utc).timestamp()*1000))
PY
)
CUT_MS=$(python3 - "$MONTHS" <<'PY'
from datetime import datetime, timezone, timedelta
import sys
months=int(sys.argv[1]); now=datetime.now(timezone.utc)
cut=now - timedelta(days=30*months)
print(int(cut.timestamp()*1000))
PY
)

# ---- fetcher for a single tf (15 or 60 only) ----
fetch_tf(){
  local tf="$1"
  local token; token="$(map_bybit_tf "${tf}")"
  if [ "${tf}" != "15" ] && [ "${tf}" != "60" ]; then
    log "[SKIP] tf=${tf} fetched via aggregation or unsupported"
    return 0
  fi
  [ "${token}" = "__BAD__" ] && { echo "[FAIL] unsupported tf=${tf}"; exit 1; }

  local out="cache/${SYMBOL}_${tf}.csv"
  local part="tmp/${SYMBOL}_${tf}.part.csv"
  : > "${part}"

  local limit=1000
  # окно запроса: limit*tf минут
  local step_ms=$(( tf * 60 * 1000 * limit ))
  local end="${NOW_MS}"
  local got=0

  while [ "${end}" -gt "${CUT_MS}" ]; do
    local start=$(( end - step_ms + 1 ))
    if [ "${start}" -lt "${CUT_MS}" ]; then start="${CUT_MS}"; fi

    local url="${API}/v5/market/kline?category=${CATEGORY}&symbol=${SYMBOL}&interval=${token}&start=${start}&end=${end}&limit=${limit}"
    log "GET tf=${tf} ${start}..${end}"
    # Защита от сетевых сбоев
    set +e
    resp="$(curl -sS "${url}")"; rc=$?
    set -e
    [ $rc -ne 0 ] && { echo "[WARN] curl rc=$rc"; break; }

    # кол-во элементов
    n=$(printf "%s" "${resp}" | jq -r '.result.list|length // 0')
    [ -z "${n}" ] && n=0
    if [ "${n}" -eq 0 ]; then
      log "no data; stop"
      break
    fi

    # нормализуем: ts,open,high,low,close,volume ; сортировка по ts
    printf "%s" "${resp}" \
    | jq -r '
        .result.list
        | map([ (.[0]|tonumber),(.[1]|tonumber),(.[2]|tonumber),
                (.[3]|tonumber),(.[4]|tonumber),(.[5]|tonumber) ])
        | sort_by(.[0])
        | .[]
        | @csv
      ' >> "${part}"

    got=1
    # следующий шаг — до самой ранней свечи минус 1мс
    end=$(printf "%s" "${resp}" | jq -r '.result.list | map(.[0]|tonumber) | min - 1')
    [ -z "${end}" ] && break

    # лёгкий троттлинг
    sleep 0.15
  done

  [ "${got}" -eq 0 ] && { echo "[FAIL] fetched 0 rows for ${tf}m"; return 1; }

  # слить, дедуп по ts, отсортировать
  awk -F',' '!seen[$1]++' "${part}" | sort -t',' -n -k1,1 > "${out}"
  rows=$(wc -l < "${out}" || echo 0)
  log "WROTE ${out} rows=${rows}"
}

# ---- aggregation 60 -> 240,1440 ----
agg_from_60(){
  local base="cache/${SYMBOL}_60.csv"
  [ -s "${base}" ] || { echo "[FAIL] base 60m missing"; return 1; }

  # generic aggregator: period minutes -> ms bin
  _agg(){
    local period_min="$1" out="cache/${SYMBOL}_${period_min}.csv"
    awk -F',' -v P_MS="$(( $1 * 60 * 1000 ))" '
      function flush(){ if(n){ printf("%s,%.10f,%.10f,%.10f,%.10f,%.10f\n", t0,o,h,l,c,v); } }
      BEGIN{OFS=","; n=0}
      {
        t=$1+0; key=int(t/P_MS)
        if(key!=cur){
          flush(); cur=key; t0=key*P_MS;
          o=$2+0; h=$3+0; l=$4+0; c=$5+0; v=$6+0; n=1
        } else {
          if($3+0>h)h=$3+0; if($4+0<l)l=$4+0; c=$5+0; v+=($6+0)
        }
      }
      END{ flush() }
    ' "$2" > "${out}"
    [ -s "${out}" ] || { echo "[FAIL] ${out} empty"; return 1; }
    log "AGG ${out} rows=$(wc -l < "${out}")"
  }
  _agg 240 "${base}"
  _agg 1440 "${base}"
}

# ---- main ----
need60=0
need15=0
for tf in ${INTERVALS}; do
  case "${tf}" in
    15)  need15=1 ;;
    60)  need60=1 ;;
    240|1440) need60=1 ;;
    *) echo "[FAIL] unsupported tf=${tf}"; exit 1 ;;
  esac
done

[ "${need60}" -eq 1 ] && fetch_tf 60
[ "${need15}" -eq 1 ] && fetch_tf 15
[ "${need60}" -eq 1 ] && agg_from_60

# clean-копии
mkdir -p cache/clean
for tf in 15 60 240 1440; do
  src="cache/${SYMBOL}_${tf}.csv"; dst="cache/clean/${SYMBOL}_${tf}.csv"
  [ -s "${src}" ] && { cp -f "${src}" "${dst}"; log "CLEAN -> ${dst}"; } || true
done

log "DONE"
