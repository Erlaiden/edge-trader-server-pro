#!/usr/bin/env bash
set -euo pipefail
SYM="${1:?symbol like ETHUSDT}"
TF="${2:?interval like 15|60|240|1440}"
OUT="/opt/edge-trader-server/cache/${SYM}_${TF}.csv"

# В этом скрипте напрямую качаем только 15m.
# Для остальных ТФ используем отдельный агрегатор (см. fetch_15m_and_agg.sh).
if [[ "$TF" != "15" ]]; then
  echo "This loader only fetches 15m. Use fetch_15m_and_agg.sh for $TF." >&2
  exit 2
fi

URL="https://api.bybit.com/v5/market/kline?category=linear&symbol=${SYM}&interval=15&limit=1000"

TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT

curl -sS "$URL" >"$TMP"
RET="$(jq -r '.retCode' "$TMP" 2>/dev/null || echo 1)"
if [[ "$RET" != "0" ]]; then
  echo "Bybit retCode=$RET for ${SYM} ${TF}" >&2
  jq -r '.retMsg?' "$TMP" 2>/dev/null || true
  exit 3
fi

# Собираем CSV: ts(ms), o,h,l,c, volume, turnover; сортируем по времени по возрастанию
{
  echo "ts,open,high,low,close,volume,turnover"
  jq -r '
    .result.list[]
    | [
        (.[0] | tonumber),      # startTime ms (оставляем ms, сервер читает как число)
        (.[1] | tonumber),
        (.[2] | tonumber),
        (.[3] | tonumber),
        (.[4] | tonumber),
        (.[5] | tonumber),
        (.[6] | tonumber)
      ] | @csv
  ' "$TMP" | sort -t, -k1,1n
} >"$OUT"

LINES=$(($(wc -l <"$OUT")-1))
echo "Wrote $OUT ($LINES lines)"
