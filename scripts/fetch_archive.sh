#!/usr/bin/env bash
set -euo pipefail

# === ВХОДНЫЕ ПАРАМЕТРЫ ===
SYMBOL="${SYMBOL:-BTCUSDT}"
MONTHS="${MONTHS:-6}"
INTERVALS="${INTERVALS:-"15 60 240 1440"}"

# Источник по умолчанию — Bybit MT4 archive
ARCHIVE_URL_TMPL="${ARCHIVE_URL_TMPL:-https://public.bybit.com/kline_for_metatrader4/{SYMBOL}/{YYYY}/{SYMBOL}_{SRC_TF}_{YYYY}-{MM}-01_{YYYY}-{MM_LAST}.csv.gz}"
ALLOWED_HOST="${ALLOWED_HOST:-public.bybit.com}"
ALLOW_ANY_HOST="${ALLOW_ANY_HOST:-0}"

# Куда кладём промежуточные месяцы и итог
PUB_DIR="public/bybit/kline_for_metatrader4/${SYMBOL}"
CACHE_DIR="cache"
RAW="${CACHE_DIR}/${SYMBOL}_%s.csv"      # raw: 7 колонок
CLEAN="${CACHE_DIR}/clean/${SYMBOL}_%s.csv"

mkdir -p "${PUB_DIR}" "${CACHE_DIR}" "${CACHE_DIR}/clean"

log(){ printf '[%s] %s\n' "$(date -u +%H:%M:%S)" "$*"; }

# === УТИЛИТЫ ДАТЫ ===
# Последний день месяца
last_day_of_month(){
  local y="$1" m="$2"
  case "$m" in
    01|03|05|07|08|10|12) echo 31;;
    04|06|09|11) echo 30;;
    02)
      # простая проверка високосного
      if (( (y%4==0 && y%100!=0) || (y%400==0) )); then echo 29; else echo 28; fi
    ;;
  esac
}

# N=0..MONTHS-1 месяцев назад от текущего UTC
month_offset(){
  python3 - <<PY
import sys,datetime,calendar
n=int(sys.argv[1])
now=datetime.datetime.now(datetime.UTC)
y=now.year; m=now.month
m -= n
while m<=0:
    m+=12; y-=1
print(y, f"{m:02d}")
PY
}

# === МАППИНГ ТФ В ИСТОЧНИК ===
# Bybit хранит: 1,5,15,30,60. 240 и 1440 делаем локально из 60м.
src_tf_for(){
  case "$1" in
    1|5|15|30|60) echo "$1" ;;
    240|1440)     echo "60" ;;
    *) echo "unsupported" ;;
  esac
}

# === СКАЧИВАНИЕ ЕЖЕМЕСЯЧНЫХ ФАЙЛОВ ===
fetch_month(){
  local tf="$1" y="$2" mm="$3"
  local src_tf; src_tf="$(src_tf_for "$tf")"
  [ "$src_tf" = "unsupported" ] && { log "SKIP unsupported TF=${tf}"; return 0; }

  local dd; dd="$(last_day_of_month "$y" "$mm")"
  local url="${ARCHIVE_URL_TMPL}"
  url="${url//\{SYMBOL\}/$SYMBOL}"
  url="${url//\{YYYY\}/$y}"
  url="${url//\{MM\}/$mm}"
  url="${url//\{MM_LAST\}/$mm-$dd}"
  url="${url//\{SRC_TF\}/$src_tf}"

  # Валидация хоста
  if [ "$ALLOW_ANY_HOST" != "1" ]; then
    host="$(printf '%s' "$url" | sed -E 's#^https?://([^/]+)/.*$#\1#')"
    if [ "$host" != "$ALLOWED_HOST" ]; then
      log "FAIL host not allowed: $host"
      return 1
    fi
  fi

  local outdir="${PUB_DIR}/${src_tf}/${y}"
  mkdir -p "$outdir"
  local fname="${SYMBOL}_${src_tf}_${y}-${mm}-01_${y}-${mm}-${dd}.csv.gz"
  local dst="${outdir}/${fname}"

  if [ -f "$dst" ]; then
    log "HIT  $dst (skip)"
    return 0
  fi

  log "GET  $url"
  if ! curl -fL --retry 3 --retry-delay 2 -sS "$url" -o "$dst"; then
    log "MISS $y-$mm tf=${tf} (no file)"
    rm -f "$dst"
    return 2
  fi

  return 0
}

# === РАЗВОРАЧИВАЕМ В raw CSV ПО TF (склейка месяцев, сортировка, дедуп) ===
inflate_concat_to_raw(){
  local tf="$1"
  local src_tf; src_tf="$(src_tf_for "$tf")"
  [ "$src_tf" = "unsupported" ] && return 0

  local tmp="$(mktemp)"
  : > "$tmp"

  # Собираем все скачанные .csv.gz за нужные месяцы
  find "${PUB_DIR}/${src_tf}" -type f -name '*.csv.gz' | sort | while read -r gz; do
    gzip -cd "$gz" >> "$tmp" || true
  done

  if [ ! -s "$tmp" ]; then
    log "FAIL no monthly files unpacked for tf=${tf} (src=${src_tf})"
    rm -f "$tmp"
    return 2
  fi

  # Нормализуем: сортировка по 1-й колонке (ts ms), удаляем дубли
  # Ожидаемый формат строк: ts,open,high,low,close,volume,quoteVolume
  awk -F',' 'NF>=6{print $0}' "$tmp" \
    | sort -t',' -k1,1n \
    | awk -F',' '!seen[$1]++' \
    > "$(printf "$RAW" "$src_tf")"

  rm -f "$tmp"

  # Если tf совпадает с источником — просто копируем
  if [ "$tf" = "$src_tf" ]; then
    cp -f "$(printf "$RAW" "$src_tf")" "$(printf "$RAW" "$tf")"
    return 0
  fi

  # Иначе агрегируем из 60м
  if [ "$tf" = "240" ] || [ "$tf" = "1440" ]; then
    local in="$(printf "$RAW" "$src_tf")"
    local out="$(printf "$RAW" "$tf")"
    local win_minutes="$tf"
    # Группируем окнами win_minutes, берём O=первый open, H=max, L=min, C=последний close, V=sum(volume)
    awk -F',' -v W="$win_minutes" '
      function flush(){
        if(n>0){
          printf "%d,%.10f,%.10f,%.10f,%.10f,%.10f\n", T*60000, O,H,L,C,V;
        }
        n=0
      }
      {
        ts=$1; o=$2; h=$3; l=$4; c=$5; v=$6;
        # база минут
        base = int(ts/60000);
        Tbin = int(base/W); # номер окна
        if(n==0){ T=Tbin; O=o; H=h; L=l; C=c; V=v; n=1; }
        else if(Tbin!=T){ flush(); T=Tbin; O=o; H=h; L=l; C=c; V=v; n=1; }
        else { if(h>H)H=h; if(l<L)L=l; C=c; V+=v; n++; }
      }
      END{ flush(); }
    ' "$in" > "$out"
  fi
}

# === ПРОСТОЙ CLEAN (6 колонок, без quoteVolume), зеркалим naming ===
make_clean(){
  local tf="$1"
  local in="$(printf "$RAW"  "$tf")"
  local out="$(printf "$CLEAN" "$tf")"
  if [ ! -s "$in" ]; then
    log "SKIP clean tf=${tf} (no raw)"
    return 0
  fi
  awk -F',' 'NF>=6{printf "%s,%.10f,%.10f,%.10f,%.10f,%.10f\n",$1,$2,$3,$4,$5,$6}' "$in" > "$out"
}

# === MAIN ===
# 1) Скачиваем помесячно за N месяцев назад для каждого TF (по источнику/маппингу)
for tf in $INTERVALS; do
  for ((i=0;i<MONTHS;i++)); do
    read -r YY MM < <(month_offset "$i")
    fetch_month "$tf" "$YY" "$MM" || true
  done
done

# 2) Склейка в raw и возможная агрегация 240/1440 из 60м
for tf in $INTERVALS; do
  inflate_concat_to_raw "$tf" || true
done

# 3) Готовим clean
for tf in $INTERVALS; do
  make_clean "$tf" || true
done

# 4) Краткий отчёт
for tf in $INTERVALS; do
  f_raw="$(printf "$RAW" "$tf")"
  f_clean="$(printf "$CLEAN" "$tf")"
  if [ -f "$f_raw" ];  then head -n1 "$f_raw"   | awk -v F="$f_raw"  -F',' '{printf("RAW  %s  cols=%d sample=%s\n",F,NF,$0)}'; fi
  if [ -f "$f_clean" ];then head -n1 "$f_clean" | awk -v F="$f_clean" -F',' '{printf("CLEAN%s  cols=%d sample=%s\n",F,NF,$0)}'; fi
done
