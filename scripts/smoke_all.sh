#!/usr/bin/env bash
set -euo pipefail

PORT="${PORT:-3900}"
BASE="http://127.0.0.1:${PORT}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")"/.. && pwd)"
LOG_FILE="${ROOT_DIR}/smoke_server.log"

cmake -S "$ROOT_DIR" -B "$ROOT_DIR/build"
cmake --build "$ROOT_DIR/build" -j"$(nproc)"

if [[ "${EDGE_SYMBOL_QUEUE_FAKE:-}" == "1" ]]; then
  export EDGE_SYMBOL_QUEUE_FAKE=1
fi

"$ROOT_DIR/build/edge_trader_server" "$PORT" >"$LOG_FILE" 2>&1 &
SERVER_PID=$!
trap 'kill "$SERVER_PID" 2>/dev/null || true' EXIT

for _ in {1..60}; do
  if curl -sS "$BASE/api/health" | jq -e '.ok == true' >/dev/null 2>&1; then
    break
  fi
  sleep 1
  if ! kill -0 "$SERVER_PID" >/dev/null 2>&1; then
    echo "server terminated prematurely" >&2
    exit 1
  fi
  if [[ $_ -eq 60 ]]; then
    echo "server did not become ready" >&2
    exit 1
  fi
done

echo "== enqueue hydrate tasks =="
read -r -d '' PAYLOAD <<'JSON'
{
  "symbol": "BTCUSDT",
  "tasks": [
    {"interval": "15m",   "months": 6},
    {"interval": "60m",   "months": 6},
    {"interval": "240m",  "months": 6},
    {"interval": "1440",  "months": 12}
  ]
}
JSON
curl -sS -f -X POST -H "Content-Type: application/json" \
  "$BASE/api/symbol/hydrate" -d "$PAYLOAD" | tee "$ROOT_DIR/smoke_hydrate.json" | jq '{symbol, tasks: [.tasks[] | {task_id, state, backfill: {interval: .backfill.interval, ok: .backfill.ok}}]}'

final_status=""
for _ in {1..180}; do
  status_json=$(curl -sS -f "$BASE/api/symbol/status?symbol=BTCUSDT")
  failed_count=$(echo "$status_json" | jq '[.tasks[] | select(.state == "failed")] | length')
  if [[ "$failed_count" -gt 0 ]]; then
    echo "$status_json" | jq '.'
    echo "hydrate task failed" >&2
    exit 1
  fi
  done_count=$(echo "$status_json" | jq '[.tasks[] | select(.state == "done")] | length')
  echo "$status_json" | jq '{tasks: [.tasks[] | {task_id, state, interval: .backfill.interval, rows: .backfill.rows}]}'
  if [[ "$done_count" -ge 4 ]]; then
    final_status="$status_json"
    break
  fi
  sleep 5
  if ! kill -0 "$SERVER_PID" >/dev/null 2>&1; then
    echo "server stopped unexpectedly" >&2
    exit 1
  fi
  if [[ $_ -eq 180 ]]; then
    echo "hydrate tasks timed out" >&2
    exit 1
  fi
done

echo "$final_status" | jq 'select(all(.tasks[]; (.backfill.ok == true) and ((.backfill.rows | tonumber) > 0) and ((.backfill.skipped_rows | tonumber) >= 0)))' >/dev/null || {
  echo "backfill validation failed" >&2
  echo "$final_status" | jq '.'
  exit 1
}

metrics=$(curl -sS -f "$BASE/api/symbol/metrics")
echo "$metrics" | jq '{enqueued_total, running, succeeded_total, failed_total, queue_length}'
echo "$metrics" | jq 'select((.succeeded_total | tonumber) >= 4 and (.failed_total | tonumber) == 0 and (.queue_length | tonumber) == 0)' >/dev/null || {
  echo "metrics validation failed" >&2
  exit 1
}

health=$(curl -sS -f "$BASE/api/health/ai")
echo "$health" | jq '{ok, data: (.data.data // [])}'
echo "$health" | jq 'select(all((.data.data // [])[]; (.clean.cols == 6) and ((.clean.rows | tonumber) >= (if .interval == "15" then 300 else 0 end))))' >/dev/null || {
  echo "health validation failed" >&2
  exit 1
}

echo "== individual tasks =="
for id in $(echo "$final_status" | jq -r '.tasks[].task_id'); do
  curl -sS -f "$BASE/api/symbol/task?id=$id" | jq '{task: .task.task_id, state: .task.state, rows: .task.backfill.rows}'
  curl -sS -f "$BASE/api/symbol/task?id=$id" | jq -e '.task.backfill.ok == true' >/dev/null || {
    echo "task $id failed validation" >&2
    exit 1
  }
done

echo "== smoke_all complete =="
