#include "utils.h"
#include "routes/symbol_queue.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <map>

using namespace etai;

int main() {
  // canonical interval tests
  assert(canonical_interval("15") == "15");
  assert(canonical_interval("15m") == "15");
  assert(canonical_interval("1H") == "60");
  assert(canonical_interval("4h") == "240");
  assert(canonical_interval("D") == "1440");
  assert(canonical_interval("bad").empty());

  // timestamp parsing tests
  long long ts = 0;
  assert(parse_timestamp_token("\xEF\xBB\xBF12345", ts) && ts == 12345);
  assert(parse_timestamp_token(" 67890 ", ts) && ts == 67890);
  assert(!parse_timestamp_token("12ab", ts));

  // cache reading with skipped lines
  std::filesystem::create_directories("tmp_test_cache");
  {
    std::ofstream f("tmp_test_cache/sample.csv");
    f << "123,foo\n";
    f << "invalid_line\n";
    f << "456,bar\n";
  }
  std::map<long long, std::string> data;
  size_t skipped = read_cache("tmp_test_cache/sample.csv", data);
  assert(skipped == 1);
  assert(data.size() == 2);
  assert(data.begin()->first == 123);
  assert(data.rbegin()->first == 456);

  // queue snapshot test with stub executor
  auto& queue = SymbolHydrateQueue::instance();
  queue.clear_for_tests();
  queue.set_executor([](const std::string& symbol, const std::string& interval, int months) {
    BackfillStats stats;
    stats.ok = true;
    stats.symbol = symbol;
    stats.interval = interval;
    stats.canonical_interval = interval;
    stats.months = months;
    stats.rows = 100;
    stats.skipped_rows = 2;
    stats.fetched_rows = 100;
    return stats;
  });

  std::string task_id = queue.enqueue("btcusdt", "15m", 6);
  queue.wait_for_idle();
  auto snap_opt = queue.snapshot_task(task_id);
  assert(snap_opt.has_value());
  auto snap = *snap_opt;
  assert(snap.state == "done");
  assert(snap.backfill.ok);
  assert(snap.backfill.rows == 100);
  assert(snap.backfill.skipped_rows == 2);
  auto metrics = queue.metrics_json();
  assert(metrics["succeeded_total"].get<std::uint64_t>() >= 1);
  assert(metrics["queue_length"].get<std::uint64_t>() == 0);

  std::filesystem::remove_all("tmp_test_cache");
  return 0;
}
