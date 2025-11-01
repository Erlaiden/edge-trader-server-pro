#!/usr/bin/env bash
set -euo pipefail
J1="$(curl -sS "http://127.0.0.1:3000/api/train?symbol=BTCUSDT&interval=15&episodes=1&tp=0.003&sl=0.0018&ma=12&flat=1")"
HN="$(jq -r '.has_norm' <<<"$J1")"
ML="$(jq -r '.mu_len'   <<<"$J1")"
SL="$(jq -r '.sd_len'   <<<"$J1")"
[ "$HN" = "true" ] && [ "$ML" = "32" ] && [ "$SL" = "32" ] || { echo "FAIL: norm missing or wrong size"; exit 1; }

J2="$(curl -sS "http://127.0.0.1:3000/api/infer?symbol=BTCUSDT&interval=15")"
UN="$(jq -r '.used_norm'      <<<"$J2")"
FD="$(jq -r '.feat_dim_used'  <<<"$J2")"
[ "$UN" = "true" ] && [ "$FD" = "32" ] || { echo "FAIL: inference not using norm or wrong feat_dim_used"; exit 1; }
echo "[OK] Norm end-to-end works (train+infer)."
