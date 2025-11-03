#!/bin/bash
set -euo pipefail

# usage (совместимость): fetch_15m_and_agg.sh <SYMBOL> [ignored_arg]
# Второй аргумент (например "12") игнорируется. Категория всегда linear (перпетуал).

SYM="${1:?usage: fetch_15m_and_agg.sh <SYMBOL>}"
CATEGORY="linear"

API="https://api.bybit.com/v5"
CACHE="/opt/edge-trader-server/cache"
CLEAN="${CACHE}/clean"
mkdir -p "${CACHE}" "${CLEAN}"

command -v curl >/dev/null || { echo "curl not found" >&2; exit 3; }
command -v jq   >/dev/null || { echo "jq not found" >&2; exit 3; }

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

RAW15="${CACHE}/${SYM}_15.csv"
RAW60="${CACHE}/${SYM}_60.csv"
RAW240="${CACHE}/${SYM}_240.csv"
RAW1440="${CACHE}/${SYM}_1440.csv"

CLN15="${CLEAN}/${SYM}_15.csv"
CLN60="${CLEAN}/${SYM}_60.csv"
CLN240="${CLEAN}/${SYM}_240.csv"
CLN1440="${CLEAN}/${SYM}_1440.csv"

LIMIT=1000
STEP_MS=$((LIMIT * 15 * 60 * 1000))   # ~10.4167 дней
NOW_S=$(date +%s); TO_MS=$((NOW_S * 1000))
FROM_S=$(date -d "-180 days" +%s); FROM_MS=$((FROM_S * 1000))

iso() { date -u -d "@$1" +"%Y-%m-%d %H:%M:%S UTC"; }

CUR_START=${FROM_MS}
MERGED_TSV="${TMPDIR}/merged15.tsv"
ITER=0
TOTAL=0

echo "Downloading ${SYM} 15m (linear) fixed window:" >&2
echo "  FROM=$(iso $((FROM_MS/1000)))  TO=$(iso $((TO_MS/1000)))" >&2

while [ "${CUR_START}" -lt "${TO_MS}" ]; do
  CUR_END=$((CUR_START + STEP_MS))
  if [ "${CUR_END}" -gt "${TO_MS}" ]; then CUR_END=${TO_MS}; fi

  echo "ITER ${ITER}: window  [$(iso $((CUR_START/1000)))  →  $(iso $((CUR_END/1000)))]" >&2

  URL="${API}/market/kline?category=${CATEGORY}&symbol=${SYM}&interval=15&limit=${LIMIT}&start=${CUR_START}&end=${CUR_END}"
  RESP="${TMPDIR}/r${ITER}.json"
  curl -sS "${URL}" -o "${RESP}"

  RET=$(jq -r '.retCode' "${RESP}" 2>/dev/null || echo 1)
  if [ "${RET}" != "0" ]; then
    echo "Bybit error retCode=${RET} $(jq -r '.retMsg//empty' "${RESP}")  window=[${CUR_START},${CUR_END}]" >&2
    exit 1
  fi

  N=$(jq -r '.result.list | length' "${RESP}")
  if [ "${N}" = "0" ] || [ "${N}" = "null" ]; then
    echo "  got 0 rows, advance window" >&2
    CUR_START=${CUR_END}; ITER=$((ITER+1)); continue
  fi

  FIRST_TS=$(jq -r '.result.list[0][0] // 0'  "${RESP}")
  LAST_TS=$(jq  -r '.result.list[-1][0] // 0' "${RESP}")
  echo "  got ${N} rows   first=$(iso $((FIRST_TS/1000)))   last=$(iso $((LAST_TS/1000)))" >&2

  jq -r '.result.list[] | @tsv' "${RESP}" >> "${MERGED_TSV}"
  TOTAL=$((TOTAL + N))

  CUR_START=${CUR_END}
  ITER=$((ITER+1))
  sleep 0.05
done

if [ ! -s "${MERGED_TSV}" ]; then
  echo "No data downloaded for ${SYM}" >&2
  exit 2
fi

# RAW 15m: ts,open,high,low,close,volume,turnover
awk -F'\t' 'NF>=7 {print $1","$2","$3","$4","$5","$6","$7}' "${MERGED_TSV}" \
  | sort -t, -k1,1n \
  | awk -F, '!seen[$1]++' > "${TMPDIR}/_raw15.csv"

# Validate numeric fields
awk -F, 'NF==7 && $1 ~ /^[0-9]+$/ && $2+0==$2 && $3+0==$3 && $4+0==$4 && $5+0==$5 && $6+0==$6 && $7+0==$7' \
  "${TMPDIR}/_raw15.csv" > "${RAW15}"

R15=$(wc -l < "${RAW15}")
echo "Wrote ${RAW15} (${R15} rows) from ${TOTAL} raw entries" >&2
if [ "${R15}" -eq 0 ]; then
  echo "Empty RAW15 — abort" >&2
  exit 2
fi

# CLEAN 15m: ts,open,high,low,close,volume
awk -F, 'NF==7 {print $1","$2","$3","$4","$5","$6}' "${RAW15}" > "${CLN15}"
echo "Wrote ${CLN15} ($(wc -l < "${CLN15}") rows)" >&2

# агрегируем 15m -> (60/240/1440)
agg_from_15m() {
  IN15="$1"; TF="$2"; OUTRAW="$3"; OUTCLN="$4"
  GROUP=$((TF/15)); if [ "${GROUP}" -lt 1 ]; then GROUP=1; fi
  awk -F, -v OFS=',' -v G="${GROUP}" '
    NR==1 {ts=$1;o=$2;h=$3;l=$4;c=$5;v=$6;n=1;next}
    {
      idx=n;bucket=int(idx/G);
      if (bucket!=prev&&n>0){print ts,o,h,l,c,v,0;v=0}
      if (bucket!=prev){ts=$1;o=$2;h=$3;l=$4;c=$5;v=$6;prev=bucket}
      else{if($3>h)h=$3;if($4<l)l=$4;c=$5;v+=($6+0)}
      n++
    }
    END{if(n>0)print ts,o,h,l,c,v,0}
  ' "${IN15}" > "${OUTRAW}"
  awk -F, -v OFS=',' '{print $1,$2,$3,$4,$5,$6}' "${OUTRAW}" > "${OUTCLN}"
}

agg_from_15m "${RAW15}" 60   "${RAW60}"   "${CLN60}"
echo "Built ${RAW60} ($(wc -l < "${RAW60}") rows)" >&2
agg_from_15m "${RAW15}" 240  "${RAW240}"  "${CLN240}"
echo "Built ${RAW240} ($(wc -l < "${RAW240}") rows)" >&2
agg_from_15m "${RAW15}" 1440 "${RAW1440}" "${CLN1440}"
echo "Built ${RAW1440} ($(wc -l < "${RAW1440}") rows)" >&2

TS_MIN=$(awk -F, 'NR==1{print $1; exit}' "${RAW15}")
TS_MAX=$(awk -F, 'END{print $1}' "${RAW15}")
echo "CSV range:  ts_min=$(iso $((TS_MIN/1000)))   ts_max=$(iso $((TS_MAX/1000)))" >&2

exit 0
