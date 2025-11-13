#!/usr/bin/env bash
set -euo pipefail

cd /opt/edge-trader-server

TS="$(date -u +'%Y%m%d-%H%M%SZ')"
MSG="snapshot: ${TS} — pre-audit"

echo "[SNAPSHOT] ${MSG}"

# 1) Проверка статуса
git status

# 2) Фиксируем все текущие файлы
git add -A
if git commit -m "${MSG}"; then
  echo "[SNAPSHOT] Commit created."
else
  echo "[SNAPSHOT] Nothing to commit, working tree clean."
fi

# 3) Тегируем снимок
TAG="srv-${TS}-pre-audit"
if git tag -a "${TAG}" -m "pre-audit snapshot"; then
  echo "[SNAPSHOT] Tag ${TAG} created."
else
  echo "[SNAPSHOT] Tag ${TAG} already exists (skip)."
fi

# 4) Пушим в текущую ветку + теги
BRANCH="$(git rev-parse --abbrev-ref HEAD)"
echo "[SNAPSHOT] Pushing to origin/${BRANCH} with tags..."
git push origin "${BRANCH}" --tags

echo "[SNAPSHOT] Done."
