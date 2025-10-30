#!/usr/bin/env bash
set -euo pipefail
PORT="${PORT:-3000}"

echo "[i] Restarting service…"
sudo systemctl restart edge-trader-server

# ждём порт до 10 сек
for i in {1..40}; do
  if ss -ltnp | grep -q ":$PORT"; then
    echo "[ok] Listening on :$PORT"
    exit 0
  fi
  sleep 0.25
done

echo "[fail] Port :$PORT is not listening. Logs:"
systemctl status --no-pager -l edge-trader-server || true
journalctl -u edge-trader-server -n 200 --no-pager || true

echo "[hint] Running binary in foreground to capture crash:"
echo "----- FG START -----"
./build/edge_trader_server "$PORT" || true
echo "----- FG END -----"
exit 1
