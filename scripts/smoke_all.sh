#!/usr/bin/env bash
set -euo pipefail
BASE="127.0.0.1:3000"
echo "== build =="
cmake --build build -j"$(nproc)" >/dev/null
echo "== restart =="
sudo systemctl restart edge-trader-server
sleep 2
echo "== health/ai =="
curl -sS "$BASE/api/health/ai" | jq '{ok, model_thr, model_ma_len}'
echo "== agents/run =="
curl -sS "$BASE/api/agents/run?type=breakout&symbol=BTCUSDT&interval=15&thr=0.4" | jq '{ok,agent,thr,score:(.infer.score),signal:(.infer.signal),sigma:(.infer.sigma)}'
echo "== metrics =="
curl -sS "$BASE/metrics" | egrep 'edge_model_thr|edge_model_ma_len|edge_model_feat_dim|edge_last_infer_'
echo "[PASS] smoke_all"
