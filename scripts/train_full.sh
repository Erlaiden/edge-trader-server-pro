#!/usr/bin/env bash
set -euo pipefail

# 0) Снимок
TS="$(date -u +'%Y%m%d-%H%M%SZ')"
git add -A
git commit -m "snapshot: ${TS} — before full train run" || true
git tag -a "srv-${TS}-train-start" -m "start full train run" || true

# 1) Сборка
cmake -S . -B build >/dev/null
cmake --build build -j"$(nproc)" >/dev/null

# 2) Параметры Reward v2 и режимы
export ETAI_ENABLE_ANTI_MANIP=1
export ETAI_MTF_ENABLE=1
export ETAI_FEE_PER_TRADE=0.0002
export ETAI_ALPHA_SHARPE=0.60

# 3) Тренировка (проверенные параметры)
TP=0.0044
SL=0.0016
MA=12

echo "[TRAIN] run: tp=${TP} sl=${SL} ma=${MA}"
curl -sS "http://127.0.0.1:3000/api/train?symbol=BTCUSDT&interval=15&episodes=80&tp=${TP}&sl=${SL}&ma=${MA}" \
| jq -r '
  {ok, best_thr,
   metrics:{val_accuracy, val_reward_v2, val_sharpe, val_winrate, val_drawdown, feat_cols}}'

# 4) Проверка модели на диске
jq '{feat_dim:.policy.feat_dim, has_norm:(.policy|has("norm")),
     mu_len:(.policy.norm.mu|length), sd_len:(.policy.norm.sd|length),
     best_thr:(.best_thr//null)}' cache/models/BTCUSDT_15_ppo_pro.json

# 5) Метрики Prometheus
curl -sS "http://127.0.0.1:3000/metrics" \
| egrep -E 'edge_reward_(avg|wctx)|edge_sharpe|edge_winrate|edge_drawdown' || true

echo "[DONE] Full train complete."
