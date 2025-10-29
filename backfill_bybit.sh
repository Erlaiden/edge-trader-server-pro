#!/usr/bin/env bash
set -euo pipefail

SYMBOL="${1:-BTCUSDT}"
MONTHS="${2:-6}"
INTERVALS=("15" "60" "240" "1440")

mkdir -p cache

tf_ms() {
  case "$1" in
    15)   echo $((15*60*1000)) ;;
    60)   echo $((60*60*1000)) ;;
    240)  echo $((240*60*1000)) ;;
    1440) echo $((1440*60*1000)) ;;
    *)    echo $((60*60*1000)) ;;
  esac
}

interval_param() {
  if [[ "$1" == "1440" ]]; then echo "D"; else echo "$1"; fi
}

now_ms() { date +%s%3N; }

months_ms() {
  local m="${1:-6}"
  # как в utils.h: 30.5 дней на месяц
  echo $(( m * 305 / 10 * 24 * 60 * 60 * 1000 ))
}

for TF in "${INTERVALS[@]}"; do
  OUT="cache/${SYMBOL}_${TF}.csv"
  TMP="$(mktemp)"
  > "$TMP"

  FRAME_MS=$(tf_ms "$TF")
  START=$(( $(now_ms) - $(months_ms "$MONTHS") ))
  END=$(now_ms)
  CUR="$START"

  echo "[INFO] Fetch ${SYMBOL} TF=${TF} for ~${MONTHS} months → ${OUT}"

  while [[ "$CUR" -lt "$END" ]]; do
    # Окно ~1000 свечей (как в utils.h)
    WINDOW=$(( FRAME_MS * 1000 ))
    TO=$(( CUR + WINDOW ))
    if [[ "$TO" -gt "$END" ]]; then TO="$END"; fi

    IP=$(interval_param "$TF")
    URL="https://api.bybit.com/v5/market/kline?category=linear&symbol=${SYMBOL}&interval=${IP}&start=${CUR}&end=${TO}&limit=1000"

    RES="$(curl -sS --max-time 20 "$URL" || true)"
    if [[ -z "$RES" ]]; then
      CUR=$(( CUR + WINDOW ))
      continue
    fi

    # Если retCode != 0 — пропускаем этот чанк
    RC="$(echo "$RES" | jq -r '(.retCode // 1)')"
    if [[ "$RC" != "0" ]]; then
      CUR=$(( CUR + WINDOW ))
      continue
    fi

    # Спускаемся в массив result.list и для каждой строки (которая сама массив)
    # печатаем ее элементы, склеенные запятой — БЕЗ кавычек (-r).
    echo "$RES" | jq -r '
      (.result.list // [])[]
      | map(.)                # элементы уже строки, просто берём как есть
      | join(",")
    ' >> "$TMP" || true

    # Продвигаем курсор на следующую свечу после последней полученной
    LAST_TS="$(echo "$RES" | jq -r '(.result.list // []) | .[-1]? | .[0] // empty')"
    if [[ -n "$LAST_TS" && "$LAST_TS" =~ ^[0-9]+$ ]]; then
      CUR=$(( LAST_TS + FRAME_MS ))
    else
      CUR=$(( CUR + WINDOW ))
    fi

    # Маленькая пауза, чтобы не злить API
    sleep 0.08
  done

  # merge с существующим, сортировка и уникализация по ts (колонка 1)
  if [[ -f "$OUT" ]]; then
    cat "$OUT" >> "$TMP"
  fi

  sort -t, -k1,1n "$TMP" | awk -F, 'BEGIN{prev=""} { if ($1!=prev) { print; prev=$1 } }' > "${OUT}.tmp"
  mv "${OUT}.tmp" "$OUT"
  rm -f "$TMP"

  ROWS=$(wc -l < "$OUT" | awk '{print $1}')
  echo "[OK] ${OUT} rows=${ROWS}"
done
