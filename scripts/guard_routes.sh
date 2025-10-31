#!/usr/bin/env bash
set -euo pipefail

# Проверяем только реальные исходники, исключая бэкапы/disabled/времянки
# Разрешаем регистрацию /api/health/ai только в src/routes/health_ai.cpp

viol=0
# Собираем список файлов для проверки
mapfile -t SRC < <(find src -type f \
  \( -name '*.c' -o -name '*.cc' -o -name '*.cpp' -o -name '*.inc.cpp' -o -name '*.h' -o -name '*.hpp' \) \
  ! -name '*.bak*' ! -name '*.disabled' ! -name '*~' ! -name '*.orig' ! -name '*.rej' \
  -print | sort)

# Ищем регистрации маршрута
hits=()
for f in "${SRC[@]}"; do
  if grep -q '"/api/health/ai"' "$f"; then
    hits+=("$f")
  fi
done

# Разрешён единственный файл
for f in "${hits[@]:-}"; do
  if [[ "$f" != "src/routes/health_ai.cpp" ]]; then
    echo "[VIOLATION] /api/health/ai registered in $f"
    viol=1
  fi
done

# Дополнительно проверим, что в health_ai.cpp он действительно есть
if ! grep -q '"/api/health/ai"' src/routes/health_ai.cpp; then
  echo "[VIOLATION] route missing in src/routes/health_ai.cpp"
  viol=1
fi

if [[ $viol -ne 0 ]]; then
  echo "[FAIL] route guard failed"
  exit 1
fi

echo "[OK] route guard passed"
