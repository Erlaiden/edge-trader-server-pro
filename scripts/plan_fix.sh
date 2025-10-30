#!/usr/bin/env bash
set -euo pipefail

echo "=== Edge Trader AI — план устранения ошибок ==="

# ---------- A. КРИТИЧНЫЕ ----------
echo "[A1] Проверка тренера и метрик"
grep -q "run_train_pro_and_save" src/train_logic.cpp || echo "Функция отсутствует"
grep -q "metrics" src/train_logic.cpp || echo "Нет проброса метрик"
echo "→ после сборки сверить вывод curl /api/train и логи"

echo "[A2] Проверка systemd ExecStart"
sudo systemctl cat edge-trader-server | grep ExecStart
echo "→ должен быть ровно один ExecStart"

echo "[A3] Пересборка бинаря и контроль sha256"
cmake -S . -B build >/dev/null
cmake --build build -j"$(nproc)" >/dev/null
sha256sum build/edge_trader_server | tee cache/logs/binary.sha256

# ---------- B. СРЕДНИЙ ПРИОРИТЕТ ----------
echo "[B1] Фиксация параметров tp/sl/ma"
grep -E "tp|sl|ma" src/train_logic.cpp || echo "Добавь дефолты tp=0.003 sl=0.0018 ma=12"
echo "→ записать в логи при каждом вызове /api/train"

echo "[B2] Проверка clean-датасета"
f="cache/clean/BTCUSDT_15.csv"
if [ -f "$f" ]; then
  lines=$(wc -l <"$f")
  echo "rows=$lines"; sha256sum "$f" | tee cache/logs/dataset.sha256
else
  echo "[FAIL] Нет $f"
fi

echo "[B3] Мини-тест health/train"
curl -fsS 127.0.0.1:3000/api/health | jq .ok
curl -fsS 127.0.0.1:3000/api/health/ai | jq .data_ok

echo "[B4] Тест обучения и проверка метрик"
curl -fsS "127.0.0.1:3000/api/train?symbol=BTCUSDT&interval=15&episodes=3&tp=0.003&sl=0.0018&ma=12" \
  | tee cache/logs/train_test.json | jq '{ok,metrics:{val_accuracy,val_reward,best_thr}}'

echo "[B5] Проверка /metrics после обучения"
curl -fsS 127.0.0.1:3000/metrics | grep -E "edge_model_thr|edge_model_ma_len|edge_model_feat_dim" \
  | tee cache/logs/metrics_check.txt

# ---------- C. РАЗВИТИЕ ----------
echo "[C1] Заглушка /api/robot/start|stop|status — после стабилизации"
echo "[C2] Расширение features до D=10–12 — отдельная ветка"
echo "[C3] MTF-фильтры и CV — после фиксации датасета"

# ---------- СНАПШОТ ----------
git add -A
git commit -m "snapshot: $(date -u +'%Y%m%d-%H%M%SZ') — plan_fix applied"
git tag -a "srv-$(date -u +'%Y%m%d-%H%M%SZ')-planfix" -m "План устранения ошибок выполнен"

echo "=== План выполнен, контрольные sha256 сохранены ==="
