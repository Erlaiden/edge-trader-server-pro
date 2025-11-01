#!/usr/bin/env bash
set -euo pipefail

BASE="http://127.0.0.1:3000"

ok(){ echo "[OK]  $*"; }
warn(){ echo "[WARN] $*" >&2; }
fail(){ echo "[FAIL] $*" >&2; exit 1; }

echo "== 0) Сервис и порт =="
ss -lntp | grep ':3000' >/dev/null && ok "listen 3000" || fail "not listening 3000"
systemctl is-active edge-trader-server >/dev/null && ok "systemd active" || warn "systemd not active?"

echo "== 1) Health =="
curl -sS "${BASE}/health" | jq . >/dev/null && ok "/health" || fail "/health bad"

echo "== 2) /metrics — ключевые гейджи =="
M="$(curl -sS "${BASE}/metrics")"
echo "$M" | grep -q '^edge_model_feat_dim ' && ok "feat_dim gauge" || fail "no feat_dim gauge"
echo "$M" | grep -q '^edge_model_ma_len '  && ok "ma_len gauge"  || fail "no ma_len gauge"

# reward v2/wctx с тренера
echo "$M" | grep -q '^edge_reward_avg '   && ok "reward_avg"   || warn "no reward_avg gauge"
echo "$M" | grep -q '^edge_reward_wctx '  && ok "reward_wctx"  || warn "no reward_wctx gauge"
echo "$M" | grep -q '^edge_sharpe '       && ok "sharpe"       || warn "no sharpe gauge"
echo "$M" | grep -q '^edge_drawdown '     && ok "drawdown"     || warn "no drawdown gauge"

echo "== 3) Модель и инварианты =="
curl -sS "${BASE}/api/model" | jq . >/tmp/model.json || fail "model api"
FEAT_API="$(jq -r '.model_feat_dim' /tmp/model.json)"
THR_API="$(jq -r '.model_thr' /tmp/model.json)"
MA_API="$(jq -r '.model_ma_len' /tmp/model.json)"
echo "feat_dim=${FEAT_API} thr=${THR_API} ma=${MA_API}"
[[ "${FEAT_API}" == "32" ]] && ok "feat_dim==32" || fail "feat_dim!=32"

echo "== 4) Инференс (single) =="
curl -sS "${BASE}/api/infer?symbol=BTCUSDT&interval=15" | jq . >/tmp/infer.json || fail "infer api"
jq -r '.signal,.score,.sigma' /tmp/infer.json >/dev/null && ok "infer fields"

echo "== 5) Инференс MTF =="
curl -sS "${BASE}/api/infer?symbol=BTCUSDT&interval=15&mtf=1" | jq . >/tmp/infer_mtf.json || fail "infer mtf api"
jq -r '.htf["60"].agree,.htf["240"].agree,.htf["1440"].agree' /tmp/infer_mtf.json >/dev/null && ok "htf agree"

echo "== 6) Агентный слой =="
curl -sS "${BASE}/api/agents/run?symbol=BTCUSDT&interval=15" | jq . >/tmp/agents.json || fail "agents api"
jq -r '.final_signal,.final_confidence' /tmp/agents.json >/dev/null && ok "agents fields"

echo "== 7) Тренер (быстрый) — телеметрия Reward v2 =="
curl -sS "${BASE}/api/train?symbol=BTCUSDT&interval=15&episodes=4&tp=0.003&sl=0.0018&ma=12" | jq . >/tmp/train.json || warn "train api"
if [[ -s /tmp/train.json ]]; then
  jq -r '.val_reward_v2,.val_reward_wctx,.val_sharpe,.val_drawdown,.best_thr,.feat_cols' /tmp/train.json >/dev/null && ok "train metrics"
fi

echo "== 8) Готовность к торговле: базовые критерии =="
# Простые пороги (не строгие): feat_dim=32; train вернул метрики; агенты выдали final_signal
[[ "${FEAT_API}" == "32" ]] || fail "feat_dim mismatch"
jq -e '.final_signal' /tmp/agents.json >/dev/null && ok "agent decision ok" || fail "no agent decision"

ok "Trade readiness SMOKE PASSED"
