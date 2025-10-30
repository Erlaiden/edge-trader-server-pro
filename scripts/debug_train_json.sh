#!/usr/bin/env bash
set -e
curl -sS "127.0.0.1:3000/api/train?symbol=BTCUSDT&interval=15&episodes=3&tp=0.003&sl=0.0018&ma=12" -o /tmp/res.json
echo "--- raw json ---"
cat /tmp/res.json | jq
echo "--- type info ---"
cat /tmp/res.json | jq '.metrics | map_values(type)'
