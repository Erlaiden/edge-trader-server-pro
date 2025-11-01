#!/usr/bin/env bash
set -euo pipefail
SYM="${1:?symbol like ETHUSDT}"
TF="${2:?interval like 15|60|240|1440}"
OUT="/opt/edge-trader-server/cache/${SYM}_${TF}.csv"

# Bybit v5 market kline for linear perps
# Fields in result.list: startTime, open, high, low, close, volume, turnover
# Write header matching server RAW=7 cols: ts,open,high,low,close,volume,turnover
URL="https://api.bybit.com/v5/market/kline?category=linear&symbol=${SYM}&interval=${TF}&limit=1000"

TMP="$(mktemp)"
curl -sS "$URL" >"$TMP"
RET="$(jq -r '.retCode' "$TMP" 2>/dev/null || echo 1)"
if [[ "$RET" != "0" ]]; then
  echo "Bybit retCode=$RET for ${SYM} ${TF}" >&2
  jq -r '.retMsg?' "$TMP" 2>/dev/null || true
  rm -f "$TMP"; exit 2
fi

# Normalize to ascending time
jq -r '
  .result.list
  | map([ (.[0]|tonumber), (.[1]|tonumber), (.[2]|tonumber), (.[3]|tonumber),
          (.[4]|tonumber), (.[5]|tonumber), (.[6]|tonumber) ])
  | sort_by(.[0])
  | (["ts","open","high","low","close","volume","turnover"]), (.[] | @csv)
' "$TMP" > "$OUT"

rm -f "$TMP"
echo "Wrote ${OUT} ($(wc -l < "$OUT") lines)"
