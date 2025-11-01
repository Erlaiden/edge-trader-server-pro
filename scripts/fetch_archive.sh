#!/usr/bin/env bash
set -euo pipefail

# === INPUT ===
SYMBOL="${SYMBOL:-BTCUSDT}"
MONTHS="${MONTHS:-6}"
INTERVALS="${INTERVALS:-"15 60 240 1440"}"

# Bybit MT4 minutes archive (SRC_TF in {1,5,15,30,60}); 240/1440 aggregate from 60
DEFAULT_TMPL='https://public.bybit.com/kline_for_metatrader4/{SYMBOL}/{YYYY}/{SYMBOL}_{SRC_TF}_{YYYY}-{MM}-01_{YYYY}-{MM_LAST}.csv.gz'
ARCHIVE_URL_TMPL="${ARCHIVE_URL_TMPL:-$DEFAULT_TMPL}"
ALLOWED_HOST="${ALLOWED_HOST:-public.bybit.com}"
ALLOW_ANY_HOST="${ALLOW_ANY_HOST:-0}"

log(){ printf '[%s] %s\n' "$(date -u +%H:%M:%S)" "$*"; }

# Sanity guard on template (kill legacy leftovers)
if [[ "$ARCHIVE_URL_TMPL" == *"{INTERVAL}"* ]] || [[ "$ARCHIVE_URL_TMPL" == *"{YYYY}-{MM}.csv.gz"* ]]; then
  log "WARN old template detected; overriding."
  ARCHIVE_URL_TMPL="$DEFAULT_TMPL"
fi
if echo "$ARCHIVE_URL_TMPL" | grep -Eq '\{YYYY\}.+/\{YYYY\}'; then
  log "WARN suspicious template; overriding."
  ARCHIVE_URL_TMPL="$DEFAULT_TMPL"
fi

# === PATHS ===
PUB_DIR="public/bybit/kline_for_metatrader4/${SYMBOL}"
CACHE_DIR="cache"
RAW_TMPL="${CACHE_DIR}/${SYMBOL}_%s.csv"         # 7 cols, epoch ms
CLEAN_TMPL="${CACHE_DIR}/clean/${SYMBOL}_%s.csv" # 6 cols
mkdir -p "${PUB_DIR}" "${CACHE_DIR}/clean"

# === DATE UTILS ===
last_day_of_month(){ local y="$1" m="$2"; case "$m" in
  01|03|05|07|08|10|12) echo 31;;
  04|06|09|11) echo 30;;
  02) if (( (y%4==0 && y%100!=0) || (y%400==0) )); then echo 29; else echo 28; fi;;
esac; }

month_offset(){ python3 - "$1" <<'PY'
import sys, datetime
i=int(sys.argv[1])
now=datetime.datetime.now(datetime.UTC)
y,m=now.year,now.month
m-=i
while m<=0: m+=12; y-=1
print(y, f"{m:02d}")
PY
}

src_tf_for(){ case "$1" in 1|5|15|30|60) echo "$1";;
  240|1440) echo "60";;
  *) echo "unsupported";; esac; }

render_url(){
  local tf="$1" y="$2" mm="$3"
  local src_tf; src_tf="$(src_tf_for "$tf")"
  [[ "$src_tf" == "unsupported" ]] && echo "" && return 0
  local dd; dd="$(last_day_of_month "$y" "$mm")"
  local url="$ARCHIVE_URL_TMPL"
  url="${url//\{SYMBOL\}/$SYMBOL}"
  url="${url//\{YYYY\}/$y}"
  url="${url//\{MM\}/$mm}"
  url="${url//\{MM_LAST\}/$mm-$dd}"
  url="${url//\{SRC_TF\}/$src_tf}"
  if [[ "$url" == *"{"* ]] || [[ "$url" == *"}"* ]]; then
    log "FAIL template not fully rendered: $url"; exit 2
  fi
  if echo "$url" | grep -Eq '\.csv\.gz/.*\.csv\.gz'; then
    log "FAIL duplicated tail after .csv.gz: $url"; exit 2
  fi
  echo "$url"
}

fetch_month(){
  local tf="$1" y="$2" mm="$3"
  local url; url="$(render_url "$tf" "$y" "$mm")"
  local host; host="$(printf '%s' "$url" | sed -E 's#^https?://([^/]+)/.*$#\1#')"
  if [[ "$ALLOW_ANY_HOST" != "1" && "$host" != "$ALLOWED_HOST" ]]; then
    log "FAIL host not allowed: $host"; return 1
  fi
  local src_tf; src_tf="$(src_tf_for "$tf")"
  local dd; dd="$(last_day_of_month "$y" "$mm")"
  local outdir="${PUB_DIR}/${src_tf}/${y}"
  local fname="${SYMBOL}_${src_tf}_${y}-${mm}-01_${y}-${mm}-${dd}.csv.gz"
  local dst="${outdir}/${fname}"
  mkdir -p "$outdir"
  if [[ -f "$dst" ]]; then log "HIT  $dst"; return 0; fi
  log "GET  $url"
  if ! curl -fL --retry 3 --retry-delay 2 -sS "$url" -o "$dst"; then
    log "MISS ${y}-${mm} tf=${tf}"
    rm -f "$dst"
    return 2
  fi
}

# Convert MT4 datetime ("YYYY.MM.DD HH:MM") → epoch ms, keep numeric OHLCV
normalize_to_epoch(){
  # stdin: raw concatenated monthly CSVs
  python3 - <<'PY'
import sys, datetime
utc=datetime.timezone.utc
for ln in sys.stdin:
    ln=ln.strip()
    if not ln: continue
    parts=ln.split(',')
    if len(parts) < 6: continue
    # If first field already looks like integer epoch -> passthrough
    if parts[0].isdigit():
        print(','.join(parts[:6])); continue
    # Expect "YYYY.MM.DD HH:MM"
    try:
        dt=datetime.datetime.strptime(parts[0], "%Y.%m.%d %H:%M").replace(tzinfo=utc)
        ts=int(dt.timestamp()*1000)
        o,h,l,c,v = parts[1:6]
        print(f"{ts},{o},{h},{l},{c},{v}")
    except Exception:
        # Try "YYYY.MM.DD H:MM"
        try:
            dt=datetime.datetime.strptime(parts[0], "%Y.%m.%d %H:%M").replace(tzinfo=utc)
            ts=int(dt.timestamp()*1000)
            o,h,l,c,v = parts[1:6]
            print(f"{ts},{o},{h},{l},{c},{v}")
        except Exception:
            continue
PY
}

inflate_concat_to_raw(){
  local tf="$1" src_tf; src_tf="$(src_tf_for "$tf")"
  [[ "$src_tf" == "unsupported" ]] && return 0
  local tmp_concat; tmp_concat="$(mktemp)"
  : > "$tmp_concat"
  if compgen -G "${PUB_DIR}/${src_tf}/*/*.csv.gz" > /dev/null; then
    # concatenate all months
    find "${PUB_DIR}/${src_tf}" -type f -name '*.csv.gz' | sort | while read -r gz; do
      gzip -cd "$gz" >> "$tmp_concat" || true
    done
  fi
  # Normalize timestamps to epoch ms
  local tmp_norm; tmp_norm="$(mktemp)"
  if [[ -s "$tmp_concat" ]]; then
    normalize_to_epoch < "$tmp_concat" > "$tmp_norm" || true
  fi
  rm -f "$tmp_concat"
  if [[ ! -s "$tmp_norm" ]]; then
    log "FAIL no monthly files unpacked for tf=${tf} (src=${src_tf})"
    rm -f "$tmp_norm"
    return 2
  fi
  local raw_src; raw_src="$(printf "$RAW_TMPL" "$src_tf")"
  awk -F',' 'NF>=6{print $0}' "$tmp_norm" \
    | sort -t',' -k1,1n \
    | awk -F',' '!seen[$1]++' \
    > "$raw_src"
  rm -f "$tmp_norm"

  local raw_tf; raw_tf="$(printf "$RAW_TMPL" "$tf")"
  if [[ "$tf" == "$src_tf" ]]; then
    # avoid cp same-file warning
    if [[ "$raw_src" != "$raw_tf" ]]; then cp -f "$raw_src" "$raw_tf"; fi
    return 0
  fi

  # Aggregate from minutes to 240/1440 (W in minutes)
  if [[ "$tf" == "240" || "$tf" == "1440" ]]; then
    awk -F',' -v W="$tf" '
      BEGIN{ OFS=","; have=0; }
      function flush(){ if(have){ printf "%d,%.10f,%.10f,%.10f,%.10f,%.10f\n", T0, O,H,L,C,V; have=0; } }
      {
        ts=$1+0; o=$2+0; h=$3+0; l=$4+0; c=$5+0; v=$6+0;
        # bin start in ms: floor to W-minute boundary
        bin_ms = int(ts/60000/W)*W*60000;
        if(!have){ T0=bin_ms; O=o; H=h; L=l; C=c; V=v; have=1; next; }
        if(bin_ms!=T0){ flush(); T0=bin_ms; O=o; H=h; L=l; C=c; V=v; have=1; }
        else { if(h>H)H=h; if(l<L)L=l; C=c; V+=v; }
      }
      END{ flush(); }
    ' "$raw_src" > "$raw_tf"
  fi
}

make_clean(){
  local tf="$1"
  local in ; in="$(printf "$RAW_TMPL"  "$tf")"
  local out; out="$(printf "$CLEAN_TMPL" "$tf")"
  if [[ ! -s "$in" ]]; then log "SKIP clean tf=${tf} (no raw)"; return 0; fi
  awk -F',' 'NF>=6{printf "%s,%.10f,%.10f,%.10f,%.10f,%.10f\n",$1,$2,$3,$4,$5,$6}' "$in" > "$out"
}

log "TEMPLATE: $ARCHIVE_URL_TMPL"
# Fetch last N months for each requested TF (allow 404 for current month)
for tf in $INTERVALS; do
  for ((i=0;i<MONTHS;i++)); do
    read -r YY MM < <(month_offset "$i")
    fetch_month "$tf" "$YY" "$MM" || true
  done
done

# Inflate → normalize → raw per TF; aggregate 240/1440 from 60
for tf in $INTERVALS; do inflate_concat_to_raw "$tf" || true; done
for tf in $INTERVALS; do make_clean "$tf" || true; done

# Quick samples (should show integer epoch ms in col1)
for tf in $INTERVALS; do
  f_raw="$(printf "$RAW_TMPL" "$tf")"
  f_clean="$(printf "$CLEAN_TMPL" "$tf")"
  [[ -f "$f_raw"  ]]  && head -n1 "$f_raw"   | awk -v F="$f_raw"   -F',' '{printf("RAW   %s  cols=%d sample=%s\n",F,NF,$0)}'
  [[ -f "$f_clean" ]] && head -n1 "$f_clean" | awk -v F="$f_clean" -F',' '{printf("CLEAN %s  cols=%d sample=%s\n",F,NF,$0)}'
done
