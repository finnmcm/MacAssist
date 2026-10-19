#include "sweep/crawler.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_set>

#include "index/database.hpp"
#include "sweep/dir_filter.hpp"
#include "sweep/indexer.hpp"
#include "sweep/politeness.hpp"
#include "sweep/sweep_status.hpp"

namespace fs = std::filesystem;

namespace macassist {

void CrawlIncremental(Indexer& idx, Database& db,
                      const std::vector<std::string>& roots,
                      SweepStatus& status, const std::atomic<bool>& stop,
                      const Politeness& politeness,
                      const std::unordered_set<std::string>& folder_roots) {
  std::string err;
  const long long gen = idx.ReadGen() + 1;

  // One transaction, committed in batches: results become searchable during
  // the initial crawl and the WAL stays bounded, without per-file fsync cost.
  db.Exec("BEGIN", &err);
  long long in_batch = 0;
  auto maybe_commit = [&]() {
    if (++in_batch >= 1000) {
      db.Exec("COMMIT", &err);
      db.Exec("BEGIN", &err);
      in_batch = 0;
    }
  };

  // Every so often, back off while the machine is under thermal pressure:
  // flush progress, then sleep in short slices until it clears (or stop).
  long long since_check = 0;
  auto maybe_throttle = [&]() {
    if (++since_check < 256) return;
    since_check = 0;
    if (!politeness.ShouldPauseSweep()) return;
    db.Exec("COMMIT", &err);
    std::fprintf(stderr, "crawler: thermal pressure -- pausing sweep\n");
    while (politeness.ShouldPauseSweep() && !stop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    db.Exec("BEGIN", &err);
    in_batch = 0;
  };

  for (const auto& root : roots) {
    if (stop.load()) break;
    const long long root_id = idx.EnsureRoot(root);
    const bool index_folders = folder_roots.count(root) > 0;

    std::error_code ec;
    auto it = fs::recursive_directory_iterator(
        root, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
      std::fprintf(stderr, "crawler: skipping root %s: %s\n", root.c_str(),
                   ec.message().c_str());
      continue;
    }
    const fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
      if (stop.load()) break;
      if (ec) {  // unreadable entry: skip, keep going
        ec.clear();
        continue;
      }
      const fs::directory_entry& e = *it;
      std::error_code sec;

      if (e.is_directory(sec)) {
        const std::string dname = e.path().filename().string();
        if (IsExcludedDir(dname)) {
          it.disable_recursion_pending();
          continue;
        }
        if (index_folders) {
          // Index the directory itself as a folder row. A package bundle is a
          // single entry: index it, but don't descend into its internals.
          const bool bundle = IsPackageBundle(dname);
          status.scanned.fetch_add(1, std::memory_order_relaxed);
          const Indexer::Result r =
              idx.UpsertDir(e.path().string(), root_id, gen);
          if (r == Indexer::Result::kInserted || r == Indexer::Result::kUpdated) {
            status.indexed.fetch_add(1, std::memory_order_relaxed);
          }
          if (bundle) it.disable_recursion_pending();
          maybe_commit();
          maybe_throttle();
        }
        continue;
      }
      if (!e.is_regular_file(sec)) continue;
      status.scanned.fetch_add(1, std::memory_order_relaxed);

      const Indexer::Result r = idx.UpsertFile(e.path().string(), root_id, gen);
      if (r == Indexer::Result::kInserted || r == Indexer::Result::kUpdated) {
        status.indexed.fetch_add(1, std::memory_order_relaxed);
      }
      maybe_commit();
      maybe_throttle();
    }
  }

  // A complete pass: tombstone everything not seen this generation (deleted
  // or moved away). A partial pass (stop requested) skips this so we never
  // mass-tombstone live files we simply did not reach.
  if (!stop.load()) {
    idx.TombstoneStale(gen);
    idx.WriteGen(gen);  // persist only after the pass completes
  }
  db.Exec("COMMIT", &err);
}

}  // namespace macassist
