#!/usr/bin/env bash
set -euo pipefail
cd /opt/edge-trader-server

# --- helpers ---
have() { command -v "$1" >/dev/null 2>&1; }
search_code() {
  local pattern="$1"
  if have rg; then
    rg -n --no-heading -g '!build' -e "$pattern"
  else
    grep -RIn --exclude-dir=build -E "$pattern" . || true
  fi
}
extract_routes() {
  if have rg; then
    rg -No -g '!build' '/api/[A-Za-z0-9_/\-]+' | sort -u
  else
    grep -Rho --exclude-dir=build -E '/api/[A-Za-z0-9_/\-]+' . | sort -u
  fi
}

mkdir -p audits

# 0. Версии инструментов
{
  date -u +"%Y-%m-%dT%H:%M:%SZ"
  printf "clang-tidy: "; (have run-clang-tidy && run-clang-tidy -version) || (have clang-tidy && clang-tidy --version) || echo "absent"
  printf "cppcheck: "; (have cppcheck && cppcheck --version) || echo "absent"
  printf "ripgrep: "; (have rg && rg --version | head -n1) || echo "absent"
} > audits/tooling_versions.txt 2>&1 || true

# 1. TODO/FIXME/заглушки
search_code 'TODO|FIXME|WIP|STUB|NOT_IMPLEMENTED|UNIMPLEMENTED|NotImplemented|assert\s*\(\s*false\s*\)|abort\s*\(|std::logic_error|#error|pragma message' \
  > audits/todos_and_stubs.txt || true

# 2. HTTP-заглушки и /api
search_code '501|NotImplemented|NOT_IMPLEMENTED|/api/' > audits/http_routes_and_501.txt || true
extract_routes > audits/api_routes.txt || true

# 3. clang-tidy
if have run-clang-tidy; then
  run-clang-tidy -p build > audits/clang_tidy.txt 2>&1 || true
elif have clang-tidy; then
  : > audits/clang_tidy.txt
  find . -path ./build -prune -o -name '*.cpp' -print0 | xargs -0 -I{} bash -c 'clang-tidy "{}" -p build' >> audits/clang_tidy.txt 2>&1 || true
else
  echo "clang-tidy absent" > audits/clang_tidy.txt
fi

# 4. cppcheck
if have cppcheck; then
  cppcheck --project=build/compile_commands.json --enable=all --inconclusive --xml 2> audits/cppcheck.xml || true
else
  echo "<note tool='cppcheck'>absent</note>" > audits/cppcheck.xml
fi

# 5. Символы и зависимости
if [[ -f build/edge_trader_server ]]; then
  nm -C --defined-only build/edge_trader_server > audits/symbols_defined.txt 2>/dev/null || true
  ldd build/edge_trader_server > audits/ldd_deps.txt 2>/dev/null || true
fi

# 6. Тесты
ctest --test-dir build --output-on-failure > audits/ctest.txt 2>&1 || true

# 7. Логи сервиса
journalctl -u edge-trader-server.service -n 200 --no-pager > audits/journal_tail.txt 2>&1 || true

# 8. Сводка
{
  echo "todos_and_stubs: $(wc -l < audits/todos_and_stubs.txt 2>/dev/null || echo 0)"
  echo "api_routes: $(wc -l < audits/api_routes.txt 2>/dev/null || echo 0)"
  echo "clang_tidy size(bytes): $(wc -c < audits/clang_tidy.txt 2>/dev/null || echo 0)"
  echo "cppcheck.xml size(bytes): $(wc -c < audits/cppcheck.xml 2>/dev/null || echo 0)"
} > audits/summary.txt || true
