#!/usr/bin/env bash
set -euo pipefail
BASE="http://127.0.0.1:3001"
FEE_LIST="0.0002 0.0005"
ALPHA_LIST="0.7 0.9 1.1"
LAMBDA_LIST="1.2 1.8 2.2"
BETA_LIST="0.4 0.6 0.8"
ETA_LIST="1.0 1.5 2.0"
POL_LIST="thr_only model"   # контрольные политики
TP="0.003"; SL="0.0018"
STEPS="${STEPS:-1500}"

printf "policy\tfee\talpha\tlambda\tbeta\teta\tpf\tsharpe\tdd\twinr\tequity\n"
for pol in $POL_LIST; do
  for fee in $FEE_LIST; do
    for a in $ALPHA_LIST; do
      for l in $LAMBDA_LIST; do
        for b in $BETA_LIST; do
          for e in $ETA_LIST; do
            out=$(ETAI_R_ALPHA="$a" ETAI_R_LAMBDA="$l" ETAI_R_BETA="$b" ETAI_R_ETA="$e" \
              curl -sS "${BASE}/api/train_env?steps=${STEPS}&policy=${pol}&fee=${fee}&tp=${TP}&sl=${SL}")
            pf=$(echo "$out" | jq -r '.pf // "nan"')
            sh=$(echo "$out" | jq -r '.sharpe // "nan"')
            dd=$(echo "$out" | jq -r '.max_dd // "nan"')
            wr=$(echo "$out" | jq -r '.winrate // "nan"')
            eq=$(echo "$out" | jq -r '.equity_final // "nan"')
            printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$pol" "$fee" "$a" "$l" "$b" "$e" "$pf" "$sh" "$dd" "$wr" "$eq"
          done
        done
      done
    done
  done
done
