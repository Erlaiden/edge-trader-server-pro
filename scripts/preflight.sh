#!/usr/bin/env bash
set -euo pipefail

SVC="edge-trader-server"
BASE="127.0.0.1:3000"

ok()  { echo "[OK]  $*"; }
fail(){ echo "[FAIL] $*" >&2; exit 1; }

echo "=== Edge Trader PRO — preflight full check ==="

# ---------- 1) Сборка ----------
cmake -S . -B build >/dev/null
cmake --build build -j"$(nproc)" >/dev/null && ok "Build done"

# ---------- 2) Перезапуск сервиса ----------
sudo systemctl restart "$SVC"
sleep 2
ok "Service restarted"

# ---------- 3) Guard на дублирование маршрутов ----------
./scripts/guard_routes.sh >/tmp/guard_routes.log 2>&1 || {
  cat /tmp/guard_routes.log; fail "Route guard failed"
}
ok "Routes validated"

# ---------- 4) Проверка health/ai ----------
./scripts/preflight_health.sh >/tmp/preflight_health.log 2>&1 || {
  cat /tmp/preflight_health.log; fail "Health check failed"
}
ok "Health consistency verified"

# ---------- 5) Финальный отчёт ----------
thr=$(curl -sS "$BASE/metrics" | awk '/^edge_model_thr /{print $2;exit}')
ma=$(curl -sS "$BASE/metrics" | awk '/^edge_model_ma_len /{print $2;exit}')
feat=$(curl -sS "$BASE/metrics" | awk '/^edge_model_feat_dim /{print $2;exit}')
ok "Metrics: thr=$thr ma=$ma feat=$feat"

echo "[PASS] All preflight checks OK"
