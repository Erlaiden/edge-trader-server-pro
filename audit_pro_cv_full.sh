#!/usr/bin/env bash
# Edge Trader AI — C++ PRO: полный аудит CV/PRO логики одним проходом.
# Работает из строго заданной директории.

set -Eeuo pipefail

ROOT="/opt/edge-trader-server"
cd "$ROOT"

echo "==> [CTX] Рабочая директория: $(pwd)"
echo "==> [CTX] Время: $(date -Is)"
echo "==> [CTX] Пользователь: $(whoami)"
echo

# --- sanity checks ---
need() { command -v "$1" >/dev/null 2>&1 || { echo "!! Требуется бинарь '$1' (не найден)"; exit 1; }; }
need curl
need jq
need sed
need nl
need grep
need awk

# Если есть git — полезно знать ревизию
if command -v git >/dev/null 2>&1 && [ -d .git ]; then
  echo "==> [GIT] Ревизия: $(git rev-parse --short HEAD) | ветка: $(git rev-parse --abbrev-ref HEAD)"
  echo
fi

echo "================== [0] Текущие ETAI_* переменные окружения =================="
( env | grep -E '^ETAI_' || true )
echo

echo "================== [1] Поиск реализаций CV/IS/OOS =================="
grep -RIn --line-number -E \
'walk_forward_cv|walkForward|cross.?valid|cv_|CV_|oos_|OOS|is_window|IS\b|fold' src || true
echo

echo "================== [2] PPO PRO (полные листинги) =================="
echo "----- src/ppo_pro.cpp -----"
nl -ba src/ppo_pro.cpp || true
echo
echo "----- src/ppo_pro.h -----"
nl -ba src/ppo_pro.h || true
echo
echo "----- src/ppo.cpp -----"
nl -ba src/ppo.cpp || true
echo
echo "----- src/ppo.h -----"
nl -ba src/ppo.h || true
echo

echo "================== [3] Утилиты/загрузка/границы данных (полные листинги) =================="
echo "----- src/utils.cpp -----"
nl -ba src/utils.cpp || true
echo
echo "----- src/utils.h -----"
nl -ba src/utils.h || true
echo

# Если есть отдельные файлы для fetch/data/cache — покажем
for f in src/fetch.cpp src/fetch.h src/data.cpp src/data.h src/cv.cpp src/cv.h; do
  if [ -f "$f" ]; then
    echo "----- $f -----"
    nl -ba "$f" || true
    echo
  fi
done

echo "================== [4] HTTP main.cpp (ENV, маршруты, Prometheus) =================="
nl -ba src/main.cpp || true
echo

echo "================== [5] Модель/метрики JSON (полный вывод) =================="
MODEL_JSON="cache/models/BTCUSDT_15_ppo_pro.json"
if [ -f "$MODEL_JSON" ]; then
  echo "----- $MODEL_JSON (pretty) -----"
  jq . "$MODEL_JSON" || cat "$MODEL_JSON" || true
else
  echo "Файл модели не найден: $MODEL_JSON"
fi
echo

echo "================== [6] Логи CV (последние 5, полный вывод первых двух) =================="
ls -lt cache/logs 2>/dev/null | head -n 5 || true
latest_cv_log="$(ls -1t cache/logs/pro_cv_15_* 2>/dev/null | head -n 1 || true)"
second_cv_log="$(ls -1t cache/logs/pro_cv_15_* 2>/dev/null | sed -n '2p' || true)"
echo
if [ -n "${latest_cv_log:-}" ] && [ -f "$latest_cv_log" ]; then
  echo "----- Последний лог CV: $latest_cv_log -----"
  cat "$latest_cv_log" || true
  echo
fi
if [ -n "${second_cv_log:-}" ] && [ -f "$second_cv_log" ]; then
  echo "----- Предыдущий лог CV: $second_cv_log -----"
  cat "$second_cv_log" || true
  echo
fi

echo "================== [7] Состояние API и Prometheus =================="
echo "----- Проверка, что порт 3000 отвечает (/api/health) -----"
curl -sS --max-time 5 http://127.0.0.1:3000/api/health | jq . || echo "!! /api/health не отвечает"
echo
echo "----- /api/health/ai -----"
curl -sS --max-time 5 http://127.0.0.1:3000/api/health/ai | jq . || echo "!! /api/health/ai не отвечает"
echo
echo "----- /api/model?symbol=BTCUSDT&interval=15 -----"
curl -sS --max-time 5 "http://127.0.0.1:3000/api/model?symbol=BTCUSDT&interval=15" | jq . || echo "!! /api/model не отвечает"
echo
echo "----- /metrics (первые 200 строк для быстрого обзора) -----"
curl -sS --max-time 5 http://127.0.0.1:3000/metrics | sed -n '1,200p' || echo "!! /metrics не отвечает"
echo

echo "================== [8] Репродукция бага (ETAI_CV_FOLDS=7, SEED=123) =================="
ETAI_CV_FOLDS=7 ETAI_SEED=123 \
curl -sS --max-time 60 "http://127.0.0.1:3000/api/train?symbol=BTCUSDT&interval=15&episodes=8&tp=0.008&sl=0.0032&ma=12" \
| jq '{ok, model_path, metrics: {cv_folds, is_summary, oos_summary, totalReward}}' || true
echo

echo "================== [9] Диагностика окружения/сборки =================="
echo "----- CMakeLists.txt (верх) -----"
sed -n '1,120p' CMakeLists.txt || true
echo
if [ -x build/edge_trader_server ]; then
  echo "Бинарь найден: build/edge_trader_server"
  echo "Размер: $(stat -c%s build/edge_trader_server) байт"
  echo "md5: $(md5sum build/edge_trader_server | awk '{print $1}')"
else
  echo "!! Бинарь build/edge_trader_server не найден"
fi
echo

echo "================== [10] Финал: сводка =================="
echo "* Директория: $(pwd)"
echo "* Модель: $MODEL_JSON ($( [ -f "$MODEL_JSON" ] && echo 'есть' || echo 'нет' ))"
echo "* Последний CV-лог: ${latest_cv_log:-none}"
echo "* Время окончания: $(date -Is)"
echo "================== [AUDIT COMPLETE] =================="
