#!/usr/bin/env bash
set -euo pipefail

# ---------- Параметры смоука ----------
BASE_BG="http://127.0.0.1:3001"
SYM="${SYM:-BTCUSDT}"
INT="${INT:-15}"
EPISODES="${EPISODES:-6}"
TP="${TP:-0.008}"
SL="${SL:-0.0032}"
MA="${MA:-12}"

ok()  { echo "[OK]  $*"; }
fail(){ echo "[FAIL] $*" >&2; exit 1; }

# ---------- 1) Сборка ----------
cmake -S . -B build >/dev/null
cmake --build build -j"$(nproc)" >/dev/null && ok "Build done"

# ---------- 2) Символы, которые обязаны существовать ----------
must_symbols=(
  "etai::load_cached_xy(std::string const&, std::string const&, arma::Mat<double>&, arma::Mat<double>&)"
  "etai::run_train_pro_and_save(std::string const&, std::string const&, int, double, double, int)"
  "register_train_routes(httplib::Server&)"
  "etai::init_model_atoms_from_disk(std::string const&, std::string const&)"
  "etai::get_model_thr()"
  "etai::get_model_ma_len()"
  "etai::get_model_feat_dim()"
  "etai::get_current_model()"
)

nm -C build/CMakeFiles/edge_trader_server.dir/src/*.o > /tmp/etai_nm_objects.txt || true
nm -C build/edge_trader_server                         > /tmp/etai_nm_binary.txt  || true

for sym in "${must_symbols[@]}"; do
  if grep -Fq "$sym" /tmp/etai_nm_objects.txt || grep -Fq "$sym" /tmp/etai_nm_binary.txt; then
    ok "Symbol present: $sym"
  else
    fail "Missing symbol: $sym (проверь заголовок/namespace/сигнатуру)"
  fi
done

# ---------- 3) Форграунд-запуск на 3001 ----------
/opt/edge-trader-server/build/edge_trader_server 3001 >/tmp/etai_fg.log 2>&1 &
PID=$!
sleep 0.6

trap 'kill -9 $PID >/dev/null 2>&1 || true' EXIT

# ---------- 4) Смоук эндпоинтов ----------
curl -sf "$BASE_BG/api/health/ai" >/tmp/etai_health_ai.json || fail "health/ai failed"
jq -e '.ok == true' /tmp/etai_health_ai.json >/dev/null       || fail "health/ai: ok!=true"
jq -e '.model.best_thr, .model.ma_len, .model.schema, .model.mode' /tmp/etai_health_ai.json >/dev/null || fail "health/ai: model fields missing"
ok "health/ai OK"

curl -sf "$BASE_BG/metrics" | sed -n '1,60p' >/tmp/etai_metrics.txt || fail "metrics failed"
grep -q '^edge_model_thr '      /tmp/etai_metrics.txt || fail "metrics: edge_model_thr missing"
grep -q '^edge_model_ma_len '   /tmp/etai_metrics.txt || fail "metrics: edge_model_ma_len missing"
grep -q '^edge_model_feat_dim ' /tmp/etai_metrics.txt || fail "metrics: edge_model_feat_dim missing"
ok "metrics OK"

ETAI_PRO_MODE=train ETAI_CV_FOLDS=5 ETAI_SEED=123 \
curl -sf "$BASE_BG/api/train?symbol=$SYM&interval=$INT&episodes=$EPISODES&tp=$TP&sl=$SL&ma=$MA" \
| tee /tmp/etai_train.json >/dev/null || fail "train failed"
jq -e '.ok == true' /tmp/etai_train.json >/dev/null || fail "train: ok!=true"
ok "train OK (см. /tmp/etai_train.json)"

# Если дошли сюда — всё хорошо
ok "Preflight PASSED"
