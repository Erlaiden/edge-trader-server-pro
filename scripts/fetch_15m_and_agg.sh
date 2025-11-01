#!/usr/bin/env bash
set -euo pipefail
SYM="${1:?symbol like ETHUSDT}"
# второй параметр — не обязателен; если указан — минимально требуемое число 15m-баров
NEED15="${2:-600}"  # по умолчанию ~6-7 дней (15m*600 ≈ 150 часов)

CACHEDIR="/opt/edge-trader-server/cache"
SRC15="${CACHEDIR}/${SYM}_15.csv"
OUT60="${CACHEDIR}/${SYM}_60.csv"
OUT240="${CACHEDIR}/${SYM}_240.csv"
OUT1440="${CACHEDIR}/${SYM}_1440.csv"

# -- 1) Качаем 15m (1000 свечей)
"/opt/edge-trader-server/scripts/fetch_bybit_csv.sh" "$SYM" 15

have=$(($(wc -l <"$SRC15")-1))
if (( have < NEED15 )); then
  echo "WARN: have only ${have}x15m bars (<${NEED15}). Продолжаем с тем, что есть." >&2
fi

# -- 2) Универсальная агрегация CSV (7 колонок) на шаг size_min*60*1000 (ms)
# Группируем по floor(ts/STEP)*STEP, open — первый, close — последний, high=max, low=min, volume/turnover — сумма
aggregate() {
  local in_csv="$1"
  local step_min="$2"
  local out_csv="$3"
  local step_ms=$(( step_min * 60 * 1000 ))
  awk -F, -v OFS=, -v STEP_MS="$step_ms" '
    BEGIN{
      header_out="ts,open,high,low,close,volume,turnover";
      print header_out;
    }
    NR==1 { next } # пропускаем заголовок источника
    {
      ts=$1+0; o=$2+0; h=$3+0; l=$4+0; c=$5+0; v=$6+0; t=$7+0;
      bucket=int(ts/STEP_MS)*STEP_MS;
      if (bucket!=cur) {
        # флашим предыдущую группу
        if (cnt>0) {
          print cur, open_, high_, low_, close_, vol_, turn_;
        }
        # старт новой
        cur=bucket; cnt=0;
        open_=o; high_=h; low_=l; vol_=0; turn_=0;
      }
      # аккумулируем
      if (cnt==0) { open_=o }
      if (h>high_) high_=h;
      if (l<low_)  low_=l;
      close_=c;
      vol_ += v;
      turn_+= t;
      cnt++;
    }
    END{
      if (cnt>0) {
        print cur, open_, high_, low_, close_, vol_, turn_;
      }
    }
  ' "$in_csv" >"$out_csv"
}

# -- 3) Строим 60, 240, 1440
aggregate "$SRC15" 60   "$OUT60"
aggregate "$SRC15" 240  "$OUT240"
aggregate "$SRC15" 1440 "$OUT1440"

for f in "$OUT60" "$OUT240" "$OUT1440"; do
  lines=$(($(wc -l <"$f")-1))
  echo "Built $f ($lines lines)"
done
