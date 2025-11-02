#!/usr/bin/env bash
set -euo pipefail

diff_content=$(git diff --cached || true)
if [[ -z "$diff_content" ]]; then
  diff_content=$(git diff || true)
fi
if echo "$diff_content" | grep -qiE 'applypatch|git am'; then
  echo "[guard] forbidden patch workflow detected in diff" >&2
  exit 1
fi
echo "[guard] no applypatch/git am markers found"
