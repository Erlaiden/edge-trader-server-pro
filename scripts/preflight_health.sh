#!/usr/bin/env bash
set -euo pipefail

SVC="edge-trader-server"
BASE="127.0.0.1:3000"

echo "[1/4] build"
cmake --build build -j"$(nproc)" >/dev/null

echo "[2/4] restart"
sudo systemctl restart "$SVC"
sleep 2

echo "[3/4] /api/health/ai"
HAI_JSON="$(curl -sS "$BASE/api/health/ai")"
ok=$(printf '%s' "$HAI_JSON" | jq -r '.ok')
thr=$(printf '%s' "$HAI_JSON" | jq -r '.model_thr')
ma=$(printf '%s' "$HAI_JSON" | jq -r '.model_ma_len')

[[ "$ok" == "true" ]] || { echo "[FAIL] health/ai ok!=true"; exit 1; }
[[ "$thr" != "null" && "$ma" != "null" ]] || { echo "[FAIL] model_thr/ma_len are null"; echo "$HAI_JSON"; exit 1; }
echo "[OK] health/ai thr=$thr ma=$ma"

echo "[4/4] /metrics"
METRICS="$(curl -sS "$BASE/metrics")"
m_thr=$(printf '%s' "$METRICS" | awk '/^edge_model_thr /{print $2; exit}')
m_ma=$(printf '%s' "$METRICS" | awk '/^edge_model_ma_len /{print $2; exit}')
m_feat=$(printf '%s' "$METRICS" | awk '/^edge_model_feat_dim /{print $2; exit}')

[[ "$m_thr" == "$thr" ]] || { echo "[FAIL] metrics thr mismatch: $m_thr != $thr"; exit 1; }
[[ "$m_ma" == "$ma" ]]   || { echo "[FAIL] metrics ma mismatch: $m_ma != $ma"; exit 1; }
[[ -n "$m_feat" && "$m_feat" != "0" ]] || { echo "[FAIL] feat_dim invalid: $m_feat"; exit 1; }

echo "[OK] metrics consistent (thr=$m_thr ma=$m_ma feat=$m_feat)"
echo "[PASS] preflight_health OK"
