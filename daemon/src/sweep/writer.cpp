#include "sweep/writer.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <utility>

#include "index/database.hpp"
#include "sweep/crawler.hpp"
#include "sweep/fs_watcher.hpp"
#include "sweep/indexer.hpp"

namespace fs = std::filesystem;

namespace macassist {

Writer::Writer(std::string db_path, std::vector<std::string> roots)
    : db_path_(std::move(db_path)) {
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

  // Read the last processed FSEvents id BEFORE launching the thread, while the
  // connection is still single-threaded on this one.
  const std::string v = db_->GetMeta("fsevents_last_id");
  const uint64_t since =
      v.empty() ? FsWatcher::kSinceNow
                : static_cast<uint64_t>(std::strtoull(v.c_str(), nullptr, 10));

  thread_ = std::thread([this] { Run(); });

  // Start watching now, before the initial crawl finishes. Events that arrive
  // during the crawl buffer in the queue and are applied after; re-examine is
  // idempotent, so crawl/event ordering cannot corrupt the index.
  watcher_ = std::make_unique<FsWatcher>(
      roots_,
      [this](const std::string& path, uint64_t id) {
        Enqueue({WriteCommand::Kind::kExaminePath, path, id});
      },
      [this]() { Enqueue({WriteCommand::Kind::kFullRescan, {}, 0}); });
  if (!watcher_->Start(since)) {
    std::fprintf(stderr,
                 "macassistd: FSEvents watcher failed to start; "
                 "live updates disabled\n");
  }
  return true;
}

void Writer::Stop() {
  if (watcher_) watcher_->Stop();  // no more enqueues after this
  stop_.store(true);
  cv_.notify_all();  // wake the drain loop in Run()
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
    // commands in this batch (it re-examines everything).
    status_.sweeping.store(true);
    CrawlIncremental(idx, *db_, roots_, status_, stop_);
    status_.sweeping.store(false);
  } else {
    // Per-path reconcile: stat each path; present -> upsert, gone -> tombstone.
    // New files get the last completed generation so the next full pass does
    // not mistake them for stale.
    const long long gen = idx.ReadGen();
    db_->Exec("BEGIN", &err);
    for (const auto& c : batch) {
      const long long root_id = RootIdForPath(idx, c.path);
      if (idx.UpsertFile(c.path, root_id, gen) == Indexer::Result::kMissing) {
        idx.TombstoneByPath(c.path);
      }
    }
    db_->Exec("COMMIT", &err);
  }
  if (max_id != 0) {
    db_->SetMeta("fsevents_last_id", std::to_string(max_id), &err);
  }
}

void Writer::Run() {
  Indexer idx(*db_);
  if (!idx.valid()) {
    std::fprintf(stderr, "macassistd: indexer init failed; sweeps disabled\n");
  } else {
    // Initial sweep: a batch crawl run directly on the writer thread.
    status_.sweeping.store(true);
    std::fprintf(stderr, "macassistd: initial sweep over %zu root(s)...\n",
                 roots_.size());
    CrawlIncremental(idx, *db_, roots_, status_, stop_);
    status_.sweeping.store(false);
    std::fprintf(stderr,
                 "macassistd: sweep done -- %lld scanned, %lld indexed\n",
                 status_.scanned.load(), status_.indexed.load());
  }

  // Steady state: drain FSEvents-driven commands as they arrive, parked
  // (blocked, zero CPU) on the condvar when the queue is empty.
  for (;;) {
    std::deque<WriteCommand> batch;
    {
      std::unique_lock<std::mutex> lk(mtx_);
      cv_.wait(lk, [this] { return stop_.load() || !queue_.empty(); });
      if (queue_.empty()) return;  // woken by Stop() with nothing left to do
      batch.swap(queue_);
    }
    if (idx.valid()) ApplyBatch(idx, batch);
  }
}

}  // namespace macassist
