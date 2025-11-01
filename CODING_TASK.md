# Edge Trader AI PRO-X — Codex Audit Task

## Context
Hydrate queue works but unstable. Sometimes tasks stay "running" or "failed" with null fields.  
Metrics show zeros after restart. Server sometimes exits unexpectedly after multiple hydrate/task calls.  
Goal: audit and fix queue stability.

## Requirements
1. Fix task state handling, metrics counters, safe exception propagation.
2. Ensure GET /api/symbol/task always returns non-null backfill for state in {done,failed}.
3. Prevent spontaneous process termination.
4. Provide full files:
   - src/routes/symbol_queue.h
   - src/routes/symbol_queue.cpp
   - src/routes/symbol.cpp
5. Follow same code style and public API. Do not touch utils.h or /api/symbol/status.

## Acceptance tests
- POST /api/symbol/hydrate → 202 + task_id
- GET /api/symbol/task?id=<id> → consistent states
- /api/symbol/metrics updates properly
- Server does not terminate unexpectedly
