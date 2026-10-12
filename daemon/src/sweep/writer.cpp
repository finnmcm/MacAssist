#include "sweep/writer.hpp"

#include <pthread/qos.h>

#include <cstdio>
#include <filesystem>
#include <system_error>
#include <utility>

#include "extract/extractor.hpp"
#include "extract/text_extractors.hpp"
#include "index/database.hpp"
#include "sweep/crawler.hpp"
#include "sweep/fs_watcher.hpp"
#include "sweep/indexer.hpp"

namespace fs = std::filesystem;

namespace macassist {

Writer::Writer(std::string db_path, std::vector<std::string> roots,
               std::chrono::seconds reconcile_interval, bool pretend_battery)
    : db_path_(std::move(db_path)),
      reconcile_interval_(reconcile_interval),
      politeness_(pretend_battery) {
  // Canonicalize roots so path-prefix matching lines up with the realpaths
  // FSEvents reports (e.g. /tmp -> /private/tmp). weakly_canonical resolves
  // the existing portion and leaves the rest; fall back to the raw path.
  for (const auto& r : roots) {
    std::error_code ec;
    const fs::path c = fs::weakly_canonical(r, ec);
    roots_.push_back(ec ? r : c.string());
  }
}

Writer::~Writer() {
  Stop();
  if (thread_.joinable()) thread_.join();
}

bool Writer::Start(std::string* err) {
  // Open + migrate on the calling thread so the schema exists before the
  // reader opens. After this returns, the connection is touched only by Run().
  db_ = Database::Open(db_path_, Access::ReadWrite, err);
  if (!db_) return false;

  // The extractor reads stale rows on its own ReadOnly connection and posts
  // results back through our command queue. Safe to open now: the schema
  // exists (Open migrated it). A failure just disables content extraction.
  extractor_ = std::make_unique<Extractor>(
      db_path_, politeness_, status_.sweeping,
      [this](std::shared_ptr<ExtractResult> r) {
        Enqueue({WriteCommand::Kind::kApplyExtract, {}, 0, std::move(r)});
      });
  std::string ext_err;
  if (!extractor_->Start(&ext_err)) {
    std::fprintf(stderr,
                 "macassistd: extractor failed to start (%s); content "
                 "extraction disabled\n",
                 ext_err.c_str());
    extractor_.reset();
  }

  thread_ = std::thread([this] { Run(); });

  // Startup always runs a full crawl (see Run), which is the source of truth
  // for anything that changed while the daemon was down -- so the watcher only
  // needs events from now forward, not a replay of history.
  watcher_ = std::make_unique<FsWatcher>(
      roots_,
      [this](const std::string& path, uint64_t id) {
        Enqueue({WriteCommand::Kind::kExaminePath, path, id, nullptr});
      },
      [this]() { Enqueue({WriteCommand::Kind::kFullRescan, {}, 0, nullptr}); });
  if (!watcher_->Start(FsWatcher::kSinceNow)) {
    std::fprintf(stderr,
                 "macassistd: FSEvents watcher failed to start; "
                 "live updates disabled\n");
  }
  return true;
}

void Writer::Stop() {
  if (watcher_) watcher_->Stop();    // no more kExaminePath enqueues
  if (extractor_) extractor_->Stop();  // stop + join before draining ends,
                                       // so no more kApplyExtract enqueues
  stop_.store(true);
  cv_.notify_all();  // wake the drain/reconcile loop in Run()
}

void Writer::Enqueue(WriteCommand cmd) {
  {
    std::lock_guard<std::mutex> lk(mtx_);
    queue_.push_back(std::move(cmd));
  }
  cv_.notify_all();
}

long long Writer::RootIdForPath(Indexer& idx, const std::string& path) {
  for (const auto& r : roots_) {
    if (path.size() >= r.size() && path.compare(0, r.size(), r) == 0 &&
        (path.size() == r.size() || path[r.size()] == '/')) {
      return idx.EnsureRoot(r);
    }
  }
  return 0;  // outside the roots -- shouldn't happen for FSEvents paths
}

void Writer::RunFullCrawl(Indexer& idx, const char* reason) {
  // Counters report progress of the current sweep, so reset them each pass.
  status_.scanned.store(0);
  status_.indexed.store(0);
  status_.sweeping.store(true);
  std::fprintf(stderr, "macassistd: %s sweep over %zu root(s)...\n", reason,
               roots_.size());
  CrawlIncremental(idx, *db_, roots_, status_, stop_, politeness_);
  status_.sweeping.store(false);
  last_full_pass_ = std::chrono::steady_clock::now();
  std::fprintf(stderr,
               "macassistd: %s sweep done -- %lld scanned, %lld indexed\n",
               reason, status_.scanned.load(), status_.indexed.load());
  // The crawl flagged new/changed files for extraction and the extractor was
  // parked while sweeping; wake it to drain the fresh backlog.
  if (extractor_) extractor_->Notify();
}

void Writer::ApplyBatch(Indexer& idx, const std::deque<WriteCommand>& batch) {
  bool rescan = false;
  uint64_t max_id = 0;
  for (const auto& c : batch) {
    if (c.kind == WriteCommand::Kind::kFullRescan) {
      rescan = true;
    } else if (c.event_id > max_id) {
      max_id = c.event_id;
    }
  }

  std::string err;
  if (rescan) {
    // A drop/overflow was reported: a full crawl supersedes the per-path
    // commands in this batch (it re-examines everything). Any kApplyExtract
    // results in this batch are dropped -- harmless, those rows stay flagged
    // and are re-extracted on the next pass.
    RunFullCrawl(idx, "rescan");
  } else {
    // Mixed batch: per-path reconciles (stat -> upsert or tombstone) and
    // applied extraction results, in one transaction. New files get the last
    // completed generation so the next full pass does not mistake them for
    // stale.
    const long long gen = idx.ReadGen();
    long long applied = 0;
    db_->Exec("BEGIN", &err);
    for (const auto& c : batch) {
      if (c.kind == WriteCommand::Kind::kApplyExtract) {
        if (c.extract) {
          idx.ApplyExtraction(*c.extract);
          ++applied;
        }
        continue;
      }
      const long long root_id = RootIdForPath(idx, c.path);
      if (idx.UpsertFile(c.path, root_id, gen) == Indexer::Result::kMissing) {
        idx.TombstoneByPath(c.path);
      }
    }
    db_->Exec("COMMIT", &err);
    extract_status_.extracted.fetch_add(applied, std::memory_order_relaxed);
    // Flags were just cleared (extraction) and/or set (upserts); let the
    // extractor pull the next batch or newly stale files.
    if (extractor_) extractor_->Notify();
  }
  if (max_id != 0) {
    db_->SetMeta("fsevents_last_id", std::to_string(max_id), &err);
  }
}

void Writer::Run() {
  // Background sweeps run at utility QoS so they never contend with the UI.
  pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
  std::fprintf(stderr, "macassistd: power: on_battery=%d thermal_pressure=%d\n",
               politeness_.OnBattery(), politeness_.UnderThermalPressure());

  Indexer idx(*db_);
  if (!idx.valid()) {
    std::fprintf(stderr, "macassistd: indexer init failed; sweeps disabled\n");
    last_full_pass_ = std::chrono::steady_clock::now();
  } else {
    RunFullCrawl(idx, "initial");
  }

  // Steady state: drain FSEvents-driven commands as they arrive; when idle for
  // a full reconcile interval, run a reconciliation crawl (the FSEvents-drop /
  // offline backstop). Parked (zero CPU) between events and reconciles.
  for (;;) {
    std::deque<WriteCommand> batch;
    {
      std::unique_lock<std::mutex> lk(mtx_);
      const auto deadline = last_full_pass_ + reconcile_interval_;
      cv_.wait_until(lk, deadline,
                     [this] { return stop_.load() || !queue_.empty(); });
      if (stop_.load() && queue_.empty()) return;
      if (!queue_.empty()) batch.swap(queue_);
    }
    if (!batch.empty()) {
      if (idx.valid()) ApplyBatch(idx, batch);
      continue;
    }
    // Woken by the deadline with nothing queued: time to reconcile.
    if (!stop_.load() && idx.valid()) RunFullCrawl(idx, "reconcile");
  }
}

}  // namespace macassist
