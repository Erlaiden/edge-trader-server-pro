#!/usr/bin/env bash
set -euo pipefail

# ====== Конфиг через ENV ======
SYMBOL="${SYMBOL:-BTCUSDT}"
MONTHS="${MONTHS:-6}"                       # окно ретро-месяцев, включая текущий
INTERVALS="${INTERVALS:-15 60 240 1440}"    # список ТФ через пробел

# Где лежат/складываются месячные архивы (csv или csv.gz)
LOCAL_DIR="${LOCAL_DIR:-/opt/edge-trader-server/public}"

# Публичный шаблон архива. Если пусто — скачиваний НЕ делаем, используем только локальные файлы.
# Пример: https://mirror.example.com/futures/{SYMBOL}/{INTERVAL}/{YYYY}-{MM}.csv.gz
ARCHIVE_URL_TMPL="${ARCHIVE_URL_TMPL:-}"

# Белый список для защиты от подмены
ALLOWED_HOST="${ALLOWED_HOST:-mirror.example.com}"
# Для стенда можно отключить проверку домена:
ALLOW_ANY_HOST="${ALLOW_ANY_HOST:-0}"   # 1 = отключить проверку ALLOWED_HOST

# Выходные каталоги
CACHE_DIR="cache"
CLEAN_DIR="cache/clean"

fail(){ echo "[FAIL] $*" >&2; exit 1; }
ok(){   echo "[OK]  $*"; }
info(){ echo "[INFO] $*"; }

tf_ok(){ case "$1" in 15|60|240|1440) return 0;; *) return 1;; esac; }

host_from_url(){
  python3 - "$1" <<'PY'
import sys,urllib.parse
u=sys.argv[1] if len(sys.argv)>1 else ""
try:
    print(urllib.parse.urlparse(u).hostname or "")
except:
    print("")
PY
}

months_list(){
  python3 - "$MONTHS" <<'PY'
import sys,datetime,calendar
n=int(sys.argv[1])
# Берем первый день текущего месяца в UTC
today=datetime.datetime.now(datetime.timezone.utc).replace(day=1, hour=0, minute=0, second=0, microsecond=0)
out=[]
cur=today
for _ in range(max(1,n)):
    out.append(f"{cur.year:04d}-{cur.month:02d}")
    # шаг на предыдущий месяц
    first_prev = (cur.replace(day=1) - datetime.timedelta(days=1)).replace(day=1)
    cur=first_prev
print(" ".join(reversed(out)))    # от старых к новым
PY
}

cat_maybe_gz(){ case "$1" in *.gz) gzip -cd "$1";; *) cat "$1";; esac; }

local_month_path(){
  local ym="$1" tf="$2"
  local p="${LOCAL_DIR}/${SYMBOL}/${tf}/${ym}.csv"
  local g="${p}.gz"
  if   [ -f "$p" ]; then echo "$p"
  elif [ -f "$g" ]; then echo "$g"
  else echo ""; fi
}

download_month(){
  local ym="$1" tf="$2"
  [ -n "$ARCHIVE_URL_TMPL" ] || return 0

  mkdir -p "${LOCAL_DIR}/${SYMBOL}/${tf}"

  local y="${ym%-*}" m="${ym#*-}"
  local url="${ARCHIVE_URL_TMPL}"
  url="${url//\{SYMBOL\}/$SYMBOL}"
  url="${url//\{INTERVAL\}/$tf}"
  url="${url//\{YYYY\}/$y}"
  url="${url//\{MM\}/$m}"

  local host
  host="$(host_from_url "$url")"
  if [ "$ALLOW_ANY_HOST" != "1" ]; then
    [ -n "$host" ] || fail "Bad URL: $url"
    [ "$host" = "$ALLOWED_HOST" ] || fail "URL host '$host' not in whitelist '$ALLOWED_HOST'"
  fi

  local dst="${LOCAL_DIR}/${SYMBOL}/${tf}/${ym}.csv.gz"
  if [ -f "$dst" ] || [ -f "${dst%.gz}" ]; then
    info "exists: $dst"
    return 0
  fi

  info "fetch: $url"
  curl -fSL --connect-timeout 10 --max-time 120 --retry 3 --retry-delay 2 "$url" -o "$dst"
}

build_tf(){
  local tf="$1"
  tf_ok "$tf" || fail "Bad TF: $tf"

  local tmp="$(mktemp)"
  trap 'rm -f "$tmp"' RETURN

  local got=0
  for ym in $(months_list); do
    download_month "$ym" "$tf" || true
    local mf; mf="$(local_month_path "$ym" "$tf")"
    if [ -z "$mf" ]; then
      info "missing $ym tf=$tf (skip)"
      continue
    fi
    got=1
    # удаляем заголовок если он строковый (эвристика)
    if head -n1 "$mf" | gzip -cd 2>/dev/null | head -n1 | grep -Eq '^[A-Za-z]'; then
      cat_maybe_gz "$mf" | tail -n +2 >> "$tmp"
    else
      cat_maybe_gz "$mf" >> "$tmp"
    fi
  done

  [ "$got" -eq 1 ] || fail "No monthly files for tf=$tf (SYMBOL=$SYMBOL). Provide local files or valid ARCHIVE_URL_TMPL."

  mkdir -p "$CACHE_DIR" "$CLEAN_DIR"

  local out_raw="${CACHE_DIR}/${SYMBOL}_${tf}.csv"
  local out_clean="${CLEAN_DIR}/${SYMBOL}_${tf}.csv"

  # dedup + sort по первому столбцу (ts ms)
  # аккуратнее с awk: позволим дробные ts (иногда встречаются как 1.746e+12)
  awk -F, '{
    key=$1
    if(!(key in seen)){ seen[key]=1; print $0 }
  }' "$tmp" | sort -t, -k1,1n > "$out_raw"

  # clean = первые 6 колонок
  awk -F, 'NF>=6{print $1","$2","$3","$4","$5","$6}' "$out_raw" > "$out_clean"

  # отчёт
  local rows first last
  rows=$(wc -l < "$out_raw")
  first=$(head -n1 "$out_raw" | cut -d, -f1)
  last=$(tail -n1 "$out_raw" | cut -d, -f1)

  python3 - "$rows" "$first" "$last" "$tf" <<'PY'
import sys,datetime,decimal
rows=int(sys.argv[1])
def to_int_ms(x):
    try:
        return int(decimal.Decimal(x))
    except:
        return int(float(x))
first=to_int_ms(sys.argv[2]); last=to_int_ms(sys.argv[3]); tf=sys.argv[4]
def iso(ms): return datetime.datetime.fromtimestamp(ms/1000, tz=datetime.timezone.utc).isoformat().replace('+00:00','Z')
days=(last-first)/1000/86400 if last>first else 0
months=days/30.4375
print(f"[REPORT] tf={tf} rows={rows} first={iso(first)} last={iso(last)} coverage≈{days:.2f}d (~{months:.2f}m)")
PY

  ok "tf=${tf} -> $out_raw ; $out_clean"
}

main(){
  for tf in $INTERVALS; do
    build_tf "$tf"
  done
  ok "ALL DONE (SYMBOL=$SYMBOL, MONTHS=$MONTHS, TFs=$INTERVALS)"
}
main
