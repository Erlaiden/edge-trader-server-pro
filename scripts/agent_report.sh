#!/usr/bin/env bash
set -euo pipefail

# --- Параметры по умолчанию (перекрываются ENV) ---
SYM="${SYM:-BTCUSDT}"
INT="${INT:-15}"
EPISODES="${EPISODES:-6}"
TP="${TP:-0.003}"
SL="${SL:-0.0018}"
MA="${MA:-12}"
HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-3000}"

OUT="${OUT:-cache/logs/agent_report.tsv}"
TMP="$(mktemp)"

mkdir -p "$(dirname "$OUT")"

# Шапка если файл пуст/отсутствует
if [ ! -s "$OUT" ]; then
  echo -e "ts\tsymbol\tinterval\tepisodes\ttp\tsl\tma\tfeat_cols\tval_manip_ratio\tval_manip_ratio_norm\tval_manip_flagged\tval_manip_vol\tval_lambda_eff\tval_mu_eff\tval_sharpe\tval_drawdown\tval_winrate\tval_reward_v2" > "$OUT"
fi

# Запрос тренировки
JSON="$(curl -sS "http://${HOST}:${PORT}/api/train?symbol=${SYM}&interval=${INT}&episodes=${EPISODES}&tp=${TP}&sl=${SL}&ma=${MA}")"

# Извлечение полей (jq обязательно должен быть установлен)
TS="$(date -u +'%Y-%m-%dT%H:%M:%SZ')"
symbol="$(echo "$JSON" | jq -r '.symbol // empty' 2>/dev/null || true)"
interval="$(echo "$JSON" | jq -r '.interval // empty' 2>/dev/null || true)"

# берём из metrics (гарантированно есть)
feat_cols="$(echo "$JSON" | jq -r '.metrics.feat_cols // 0')"
m_ratio="$(echo "$JSON" | jq -r '.metrics.val_manip_ratio // 0')"
m_ratio_norm="$(echo "$JSON" | jq -r '.metrics.val_manip_ratio_norm // 0')"
m_flagged="$(echo "$JSON" | jq -r '.metrics.val_manip_flagged // 0')"
m_vol="$(echo "$JSON" | jq -r '.metrics.val_manip_vol // 0')"
lam_eff="$(echo "$JSON" | jq -r '.metrics.val_lambda_eff // 0')"
mu_eff="$(echo "$JSON" | jq -r '.metrics.val_mu_eff // 0')"
sharpe="$(echo "$JSON" | jq -r '.metrics.val_sharpe // 0')"
dd="$(echo "$JSON" | jq -r '.metrics.val_drawdown // 0')"
winrate="$(echo "$JSON" | jq -r '.metrics.val_winrate // 0')"
rv2="$(echo "$JSON" | jq -r '.metrics.val_reward_v2 // 0')"

# Фоллбэк на параметры запроса, если символ/интервал не прокинуты в корне
if [ -z "$symbol" ]; then symbol="$SYM"; fi
if [ -z "$interval" ]; then interval="$INT"; fi

# Запись строки
printf "%s\t%s\t%s\t%d\t%.10f\t%.10f\t%d\t%d\t%.12f\t%.12f\t%.0f\t%.12f\t%.12f\t%.12f\t%.12f\t%.12f\t%.12f\t%.12f\n" \
  "$TS" "$symbol" "$interval" "$EPISODES" "$TP" "$SL" "$MA" "$feat_cols" "$m_ratio" "$m_ratio_norm" "$m_flagged" "$m_vol" "$lam_eff" "$mu_eff" "$sharpe" "$dd" "$winrate" "$rv2" \
  >> "$OUT"

# (опционально) ограничим лог последними 500 строками
tail -n 500 "$OUT" > "$TMP" && mv "$TMP" "$OUT"
echo "[OK] agent_report appended → $OUT"
