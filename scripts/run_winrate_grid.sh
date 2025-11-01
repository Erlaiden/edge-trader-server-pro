#!/usr/bin/env bash
set -euo pipefail

SYMBOL="${SYMBOL:-BTCUSDT}"
INTERVAL="${INTERVAL:-15}"
EPISODES="${EPISODES:-120}"        # разумный баланс скорости/стабильности оценки
OUT="cache/logs/grid_results_winrate.tsv"

# Узкая, но практичная сетка — можно расширить после первичного отбора
TP_LIST="${TP_LIST:-0.003 0.0035 0.004}"
SL_LIST="${SL_LIST:-0.0018 0.002 0.0022}"
MA_LIST="${MA_LIST:-8 12 20}"

mkdir -p cache/logs cache/models/archive

echo -e "tp\tsl\tma\tbest_thr\tval_winrate\tval_reward_v2\tval_sharpe\tval_drawdown\tM_labeled\tval_size\tmodel_path" > "$OUT"

for tp in $TP_LIST; do
  for sl in $SL_LIST; do
    for ma in $MA_LIST; do
      # Запускаем тренировку через прод-сервис (порт 3000)
      J="$(curl -sS "http://127.0.0.1:3000/api/train?symbol=${SYMBOL}&interval=${INTERVAL}&episodes=${EPISODES}&tp=${tp}&sl=${sl}&ma=${ma}&flat=1")"

      ok="$(jq -r '.ok' <<<"$J" 2>/dev/null || echo false)"
      if [ "$ok" != "true" ]; then
        echo "[WARN] train failed for tp=${tp} sl=${sl} ma=${ma}: $(jq -r '.error? // "unknown_error"' <<<"$J")" >&2
        continue
      fi

      best_thr="$(jq -r '.best_thr // 0' <<<"$J")"
      val_winrate="$(jq -r '.metrics.val_winrate // 0' <<<"$J")"
      val_reward_v2="$(jq -r '.metrics.val_reward_v2 // 0' <<<"$J")"
      val_sharpe="$(jq -r '.metrics.val_sharpe // 0' <<<"$J")"
      val_drawdown="$(jq -r '.metrics.val_drawdown // 0' <<<"$J")"
      M_labeled="$(jq -r '.metrics.M_labeled // 0' <<<"$J")"
      val_size="$(jq -r '.metrics.val_size // 0' <<<"$J")"
      model_path="$(jq -r '.model_path // ""' <<<"$J")"

      # Архивируем модель под метку winrate, чтобы не потерять лучшую
      if [ -f "$model_path" ]; then
        BN="$(basename "$model_path" .json)"
        cp -f "$model_path" "cache/models/archive/${BN}_WINR$(printf '%0.3f' "$val_winrate")_TP${tp}_SL${sl}_MA${ma}.json"
      fi

      # Запись строки результата
      printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" \
        "$tp" "$sl" "$ma" "$best_thr" "$val_winrate" "$val_reward_v2" "$val_sharpe" "$val_drawdown" "$M_labeled" "$val_size" "$model_path" \
        >> "$OUT"
    done
  done
done

# Печать ТОПа по winrate
echo
echo "=== TOP by val_winrate (desc) ==="
sort -t$'\t' -k5,5nr "$OUT" | head -n 10 | nl -w2 -s'. '

# Подсказка, как выбрать лучшую модель из архива:
#   ls -1 cache/models/archive | sort -t_ -k3,3r | head
