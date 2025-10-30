#!/usr/bin/env bash
set -euo pipefail
BASE="${BASE:-http://127.0.0.1:3000}"
SYM="${SYM:-BTCUSDT}"
INT="${INT:-15}"

h="/tmp/etai_audit_health_ai.json"
m="/tmp/etai_audit_model.json"
r="/tmp/etai_audit_model_read.json"

curl -sS "$BASE/api/health/ai" | jq -S '.model | {best_thr,ma_len,schema,mode,feat:.policy.feat_dim}' > "$h"
curl -sS "$BASE/api/model?symbol=$SYM&interval=$INT" | jq -S '.model | {best_thr,ma_len,schema,mode,feat:.policy.feat_dim}' > "$m"
curl -sS "$BASE/api/model/read?symbol=$SYM&interval=$INT" | jq -S '.model | {best_thr,ma_len,schema,mode,feat:.policy.feat_dim}' > "$r"

echo "=== DIFF health vs model ==="
if diff -u "$h" "$m"; then echo "OK: health == model"; fi

echo "=== RAW model/read ==="
cat "$r"

echo "=== METRICS snapshot ==="
curl -sS "$BASE/metrics" | grep -E 'edge_model_(thr|ma_len)\b' || true
