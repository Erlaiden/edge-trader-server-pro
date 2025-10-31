#!/usr/bin/env bash
set -euo pipefail

SVC="edge-trader-server"
BASE="127.0.0.1:3000"

pass(){ echo -e "[OK]  $*"; }
warn(){ echo -e "[WARN] $*" >&2; }
fail(){ echo -e "[FAIL] $*" >&2; exit 1; }

num_or_zero(){ awk '{printf("%.12f", ($1+0))}' <<<"$1"; }

echo "== build =="
cmake --build build -j"$(nproc)" >/dev/null

echo "== restart =="
sudo systemctl restart "$SVC"
sleep 2

echo "== health/ai =="
HAI="$(curl -sS "$BASE/api/health/ai")" || fail "health/ai unreachable"
ok=$(jq -r '.ok' <<<"$HAI")
thr=$(jq -r '.model_thr' <<<"$HAI")
ma=$(jq -r '.model_ma_len' <<<"$HAI")
[[ "$ok" == "true" ]] || fail "health/ai ok!=true"
[[ "$thr" != "null" && "$ma" != "null" ]] || { echo "$HAI"; fail "thr/ma are null"; }
thr=$(num_or_zero "$thr")
ma=$(printf '%.0f' "$(num_or_zero "$ma")")
pass "health/ai ok thr=$thr ma=$ma"

echo "== metrics (before) =="
MB="$(curl -sS "$BASE/metrics")" || fail "metrics unreachable"
ts0=$(grep -E '^edge_last_infer_ts ' <<<"$MB" | awk '{print $2+0}')
l0=$(grep -E '^edge_infer_sig_long_total ' <<<"$MB" | awk '{print $2+0}')
s0=$(grep -E '^edge_infer_sig_short_total ' <<<"$MB" | awk '{print $2+0}')
n0=$(grep -E '^edge_infer_sig_neutral_total ' <<<"$MB" | awk '{print $2+0}')
mthr=$(grep -E '^edge_model_thr ' <<<"$MB" | awk '{print $2}')
mma=$(grep -E '^edge_model_ma_len ' <<<"$MB" | awk '{print $2}')
mfeat=$(grep -E '^edge_model_feat_dim ' <<<"$MB" | awk '{print $2}')

# нормализуем
mthr=$(num_or_zero "${mthr:-0}")
mma=$(printf '%.0f' "$(num_or_zero "${mma:-0}")")
mfeat=$(printf '%.0f' "$(num_or_zero "${mfeat:-0}")")

pass "metrics-before ts=$ts0 long=$l0 short=$s0 flat=$n0 | thr=$mthr ma=$mma feat=$mfeat"

# Согласованность health/ai и metrics по thr/ma
cmp_thr=$(awk -v a="$thr" -v b="$mthr" 'BEGIN{d=a-b; if(d<0)d=-d; print (d<1e-9?"OK":"DIFF")}')
if [[ "$cmp_thr" != "OK" || "$mma" != "$ma" ]]; then
  warn "model_thr/ma mismatch: health thr=$thr ma=$ma vs metrics thr=$mthr ma=$mma"
else
  pass "thr/ma consistent"
fi

echo "== agents/run (3x breakout) =="
for i in 1 2 3; do
  R="$(curl -sS "$BASE/api/agents/run?type=breakout&symbol=BTCUSDT&interval=15&thr=$thr")" || fail "agents/run#$i"
  [[ "$(jq -r '.ok' <<<"$R")" == "true" ]] || { echo "$R"; fail "agents/run#$i not ok"; }
  echo "[run#$i] score=$(jq -r '.score' <<<"$R") signal=$(jq -r '.signal' <<<"$R") sigma=$(jq -r '.sigma' <<<"$R")"
done

echo "== metrics (after) =="
MA="$(curl -sS "$BASE/metrics")" || fail "metrics unreachable (after)"
ts1=$(grep -E '^edge_last_infer_ts ' <<<"$MA" | awk '{print $2+0}')
l1=$(grep -E '^edge_infer_sig_long_total ' <<<"$MA" | awk '{print $2+0}')
s1=$(grep -E '^edge_infer_sig_short_total ' <<<"$MA" | awk '{print $2+0}')
n1=$(grep -E '^edge_infer_sig_neutral_total ' <<<"$MA" | awk '{print $2+0}')
score1=$(grep -E '^edge_last_infer_score ' <<<"$MA" | awk '{print $2}')
sigma1=$(grep -E '^edge_last_infer_sigma ' <<<"$MA" | awk '{print $2}')
signal1=$(grep -E '^edge_last_infer_signal ' <<<"$MA" | awk '{print $2}')

# нормализация
score1=$(num_or_zero "$score1")
sigma1=$(num_or_zero "$sigma1")
signal1=$(printf '%.0f' "$(num_or_zero "$signal1")")

# инварианты
(( ts1 > ts0 )) || fail "edge_last_infer_ts not updated (ts0=$ts0 ts1=$ts1)"
(( (l1+s1+n1) >= (l0+s0+n0)+1 )) || fail "infer counters not incremented"
[[ "$sigma1" != "0.000000000000" ]] || fail "edge_last_infer_sigma stayed 0"
[[ "$signal1" == "-1" || "$signal1" == "0" || "$signal1" == "1" ]] || fail "edge_last_infer_signal out of range"

pass "metrics-after ts=$ts1 long=$l1 short=$s1 flat=$n1 | last_score=$score1 last_sigma=$sigma1 last_signal=$signal1"
echo "== RESULT =="
echo "[PASS] preflight_agents_metrics OK"
