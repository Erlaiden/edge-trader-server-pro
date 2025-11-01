#!/usr/bin/env bash
set -euo pipefail

# ---------- Параметры ----------
ROOT="/opt/edge-trader-server"
OUTDIR="${ROOT}/cache/logs/audit-$(date -u +'%Y%m%d-%H%M%SZ')"
mkdir -p "$OUTDIR"

ok()  { echo "[OK]  $*"; }
warn(){ echo "[WARN] $*" >&2; }
fail(){ echo "[FAIL] $*" >&2; exit 1; }

cd "$ROOT"

# ---------- 1) Сервис и порт ----------
{
  echo "== systemctl =="
  systemctl status edge-trader-server --no-pager || true
  echo
  echo "== listening 3000 =="
  ss -lntp | grep -E '[: ]3000(\s|$)' || true
} | tee "${OUTDIR}/service_port.txt"

# ---------- 2) Билд и бинарь ----------
{
  echo "== cmake build =="
  cmake -S . -B build
  cmake --build build -j"$(nproc)"
  echo
  echo "== binary sha256 =="
  if [ -f build/edge_trader_server ]; then
    sha256sum build/edge_trader_server
  else
    echo "binary not found"
  fi
} | tee "${OUTDIR}/build_binary.txt"

# ---------- 3) Health и Metrics ----------
{
  echo "== /health =="
  curl -sS "http://127.0.0.1:3000/health" || true
  echo
  echo "== /metrics (key lines) =="
  curl -sS "http://127.0.0.1:3000/metrics" \
   | egrep -A1 '(^edge_model_feat_dim |^edge_model_ma_len |^edge_reward_|^edge_sharpe|^edge_drawdown)' || true
} | tee "${OUTDIR}/http_endpoints.txt"

# ---------- 4) Модель и инварианты ----------
MODEL_JSON="$(ls -1 cache/models/*_ppo_pro.json 2>/dev/null | head -n1 || true)"
{
  echo "MODEL_JSON=${MODEL_JSON:-none}"
  if [ -n "${MODEL_JSON:-}" ] && [ -f "$MODEL_JSON" ]; then
    echo "== model head =="
    head -n 50 "$MODEL_JSON"
    echo
    echo "== parsed =="
    jq '{version, best_thr, ma_len, feat_dim:.policy.feat_dim}' "$MODEL_JSON" || true
  else
    echo "no model json present"
  fi
} | tee "${OUTDIR}/model_check.txt"

# Достаём ровно число feat_dim из /metrics: строка должна начинаться с метрики
FEAT_DIM_METRICS="$(curl -sS "http://127.0.0.1:3000/metrics" \
  | awk '/^edge_model_feat_dim[[:space:]]+/ {print $2; exit}')"

FEAT_DIM_MODEL="NA"
if [ -n "${MODEL_JSON:-}" ] && [ -f "$MODEL_JSON" ]; then
  FEAT_DIM_MODEL="$(jq -r '.policy.feat_dim // "NA"' "$MODEL_JSON" 2>/dev/null || echo NA)"
fi

{
  echo "feat_dim(metrics)=${FEAT_DIM_METRICS:-NA}"
  echo "feat_dim(model)=${FEAT_DIM_MODEL:-NA}"
  if [[ "${FEAT_DIM_METRICS:-}" =~ ^[0-9]+$ ]] && [ "${FEAT_DIM_MODEL:-}" != "NA" ] && [ "$FEAT_DIM_METRICS" = "$FEAT_DIM_MODEL" ]; then
    ok "feat_dim invariant OK"
  else
    fail "feat_dim invariant MISMATCH"
  fi
} | tee "${OUTDIR}/invariants.txt"

# ---------- 5) Данные ----------
{
  echo "== clean csv list =="
  ls -lh cache/clean/BTCUSDT_*.csv || true
  echo
  echo "== header sample =="
  for f in cache/clean/BTCUSDT_*.csv; do
    [ -f "$f" ] || continue
    echo "# $f"
    head -n1 "$f"
  done
} | tee "${OUTDIR}/data_check.txt"

# ---------- 6) Быстрые вызовы API ----------
{
  echo "== quick train =="
  curl -sS "http://127.0.0.1:3000/api/train?symbol=BTCUSDT&interval=15&episodes=8&tp=0.003&sl=0.0018&ma=12" | jq || true
  echo
  echo "== infer single =="
  curl -sS "http://127.0.0.1:3000/api/infer?symbol=BTCUSDT&interval=15" | jq || true
  echo
  echo "== infer MTF =="
  curl -sS "http://127.0.0.1:3000/api/infer?symbol=BTCUSDT&interval=15&htf=60,240,1440" | jq || true
  echo
  echo "== agents decision =="
  curl -sS "http://127.0.0.1:3000/api/agents/decision?symbol=BTCUSDT&interval=15&thr=0.5" | jq || true
} | tee "${OUTDIR}/api_smoke.txt"

# ---------- 7) Логи systemd ----------
journalctl -u edge-trader-server -n 400 --no-pager \
  | egrep -i '(EdgeTrader|TRAIN|PPO_PRO|warn:|error|feat_ver|feat=|wctx|Manip|policy|reload)' \
  | tee "${OUTDIR}/journal_tail.txt" >/dev/null || true

# ---------- 8) Сводка ----------
{
  echo "---- SUMMARY ----"
  grep -E 'feat_dim|OK|FAIL' "${OUTDIR}/invariants.txt" || true
  egrep -A1 '(^edge_model_feat_dim |^edge_model_ma_len )' "${OUTDIR}/http_endpoints.txt" || true
  echo "Artifacts: ${OUTDIR}"
} | tee "${OUTDIR}/SUMMARY.txt"

ok "Audit completed. See ${OUTDIR}"
