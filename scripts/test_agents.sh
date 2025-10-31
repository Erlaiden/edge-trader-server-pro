#!/usr/bin/env bash
# Edge Trader AI — agents smoke-test
set -euo pipefail

SVC="edge-trader-server"
BASE="${BASE:-127.0.0.1:3000}"
TYPES=("long" "short" "flat" "correction" "breakout")
THRS=("0.30" "0.40" "0.50")

logdir="cache/logs"
mkdir -p "$logdir"
ts="$(date -u +'%Y%m%d-%H%M%SZ')"
out_tsv="$logdir/agents_check_${ts}.tsv"
tmp_tsv="$(mktemp)"

# 0) быстрая проверка сервиса
if ! curl -sS "http://${BASE}/api/health" | jq -e '.ok==true' >/dev/null; then
  echo "[FAIL] /api/health not ok" >&2
  exit 1
fi

# заголовок отчёта
echo -e "agent\tthr\tdecision\tconfidence\tok" > "$tmp_tsv"

fails=0
for t in "${TYPES[@]}"; do
  for thr in "${THRS[@]}"; do
    url="http://${BASE}/api/agents/test?type=${t}&thr=${thr}"
    res="$(curl -sS "$url" || true)"
    ok="$(printf '%s' "$res" | jq -r '.ok // false' 2>/dev/null || echo false)"
    decision="$(printf '%s' "$res" | jq -r '.decision // "NA"' 2>/dev/null || echo NA)"
    conf="$(printf '%s' "$res" | jq -r '.confidence // "NA"' 2>/dev/null || echo NA)"

    # нормализуем числа, если jq выдал null
    [[ "$decision" == "null" ]] && decision="NA"
    [[ "$conf" == "null" ]] && conf="NA"

    echo -e "${t}\t${thr}\t${decision}\t${conf}\t${ok}" >> "$tmp_tsv"

    if [[ "$ok" != "true" || "$decision" == "NA" || "$conf" == "NA" ]]; then
      echo "[WARN] agent=${t} thr=${thr}: invalid response: $res" >&2
      fails=$((fails+1))
    fi
  done
done

# сохраняем итоговый отчёт и печатаем в колонках
cp "$tmp_tsv" "$out_tsv"
echo
echo "=== Agents smoke result (${ts}) ==="
column -t "$out_tsv" | sed '1s/^/[OK] /'

# Итоговый статус
if [[ $fails -gt 0 ]]; then
  echo "[FAIL] agents check: ${fails} cases invalid. Full TSV: $out_tsv" >&2
  exit 2
fi

echo "[PASS] agents check OK. Report: $out_tsv"
