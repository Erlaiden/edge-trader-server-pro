#!/usr/bin/env bash
set -euo pipefail

SYMBOL="${SYMBOL:-BTCUSDT}"
INTERVALS="${INTERVALS:-"15 60 240 1440"}"

mkdir -p cache/clean

clean_one() {
  local tf="$1"
  local in="cache/${SYMBOL}_${tf}.csv"
  local out="cache/clean/${SYMBOL}_${tf}.csv"
  local step_ms=$(( tf * 60 * 1000 ))

  if [[ ! -s "$in" ]]; then
    echo "[SKIP] ${in} (no data)"; return 0
  fi

  # Берём первые 6 полей (ts,O,H,L,C,Vol), фильтруем мусор, выравниваем сетку и сортируем
  # Допускаем, что во входе может быть 6 или 7+ колонок.
  awk -F',' -v OFS=',' -v step="$step_ms" '
    function isnum(x){ return (x ~ /^-?[0-9]+(\.[0-9]+)?$/) }
    NF>=6 {
      ts=$1; o=$2; h=$3; l=$4; c=$5; v=$6;
      # базовая валидация
      if (ts !~ /^[0-9]+$/) next;
      if (!isnum(o) || !isnum(h) || !isnum(l) || !isnum(c) || !isnum(v)) next;
      if (o<=0 || h<=0 || l<=0 || c<=0 || v<0) next;
      # сетка
      if (ts % step != 0) next;
      print ts,o,h,l,c,v;
    }' "$in" \
  | sort -t',' -k1,1n \
  | awk -F',' -v OFS=',' '
      NR==1{prev=-1}
      {
        if ($1==prev) next;    # убираем дубликаты
        prev=$1; print
      }' > "$out.tmp"

  local rows
  rows=$(wc -l < "$out.tmp" | tr -d ' ')
  if [[ "$rows" -eq 0 ]]; then
    rm -f "$out.tmp"
    echo "[FAIL] ${in} -> 0 rows after clean"
    return 1
  fi

  mv -f "$out.tmp" "$out"
  echo "[OK]  ${out} rows=${rows}"
}

for tf in $INTERVALS; do
  clean_one "$tf"
done
