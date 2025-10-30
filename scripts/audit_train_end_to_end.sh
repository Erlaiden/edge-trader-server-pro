#!/usr/bin/env bash
set -euo pipefail

PORT=3000
SYM="${SYM:-BTCUSDT}"
INT="${INT:-15}"
EP="${EP:-2}"
TP="${TP:-0.003}"
SL="${SL:-0.0018}"
MA="${MA:-12}"

log(){ printf "%s %s\n" "[$(date -u +'%H:%M:%SZ')]" "$*"; }

log "Rebuild"
cmake -S . -B build >/dev/null
cmake --build build -j"$(nproc)" >/dev/null

log "Restart service"
sudo systemctl restart edge-trader-server

log "Wait port :$PORT"
for i in {1..30}; do
  if ss -ltnp | grep -q ":$PORT"; then break; fi
  sleep 0.3
done
ss -ltnp | grep ":$PORT" || { journalctl -u edge-trader-server -n 120 --no-pager; echo "[FATAL] port not listening"; exit 1; }

log "Ping /api/health"
curl -sS "127.0.0.1:${PORT}/api/health" || true

log "Ping /api/health/ai (ok,data_ok)"
curl -sS "127.0.0.1:${PORT}/api/health/ai" | jq '{ok, data_ok:(.data.ok)}' || true

log "Run /api/train (short)"
OUT="/tmp/train_audit_$$.json"
curl -sS "127.0.0.1:${PORT}/api/train?symbol=${SYM}&interval=${INT}&episodes=${EP}&tp=${TP}&sl=${SL}&ma=${MA}" \
  -o "$OUT"

log "Show raw train JSON"
jq . "$OUT" || cat "$OUT"

log "Show metrics keys and types"
jq '.metrics | {keys: (keys), types: (map_values(type))}' "$OUT" || true

log "Check metrics numbers"
jq '{ok, best_thr, metrics:{val_accuracy, val_reward, M_labeled, val_size, feat_cols, raw_cols, N_rows}}' "$OUT" || true

log "Recent service logs (TRAIN/AUDIT)"
journalctl -u edge-trader-server -n 200 --no-pager | grep -E "TRAIN|AUDIT|ppo_pro_exception|unknown" || true

log "Conclusion hints:"
if jq -e '.metrics.val_accuracy|numbers' "$OUT" >/dev/null 2>&1; then
  echo "  - API returns numbers: OK (проблемы нет на роуте/логике)."
else
  echo "  - API shows nulls: проверяем, что тренер вернул метрики."
  # вытащим последнее AUDIT_PPO, если мы добавляли (иначе пропустится)
  journalctl -u edge-trader-server -n 200 --no-pager | grep AUDIT_PPO | tail -n 1 || true
fi

log "Done"
