#include "extract/extractor.hpp"

#include <pthread/qos.h>
#include <sqlite3.h>

#include <cstdio>
#include <ctime>
#include <utility>
#include <vector>

#include "extract/download_date.hpp"
#include "extract/text_extractors.hpp"
#include "index/database.hpp"
#include "sweep/politeness.hpp"

namespace macassist {
namespace {

// Files per pull. One batch is in flight at a time (lock-step), so this also
// bounds the write-command queue to ~kBatch * 100 KB of payload.
constexpr int kBatch = 16;
// Re-poll when the backlog is empty (safety net; the writer also notifies on
// new work) and the longest we wait for the writer to apply a batch.
constexpr auto kIdlePoll = std::chrono::seconds(5);
constexpr auto kApplyWait = std::chrono::seconds(30);
constexpr auto kThermalPark = std::chrono::seconds(1);
constexpr long long kBatteryFreshSecs = 7 * 24 * 60 * 60;  // 7 days

struct Row {
  long long id = 0;
  std::string path, ext, kind, fingerprint;
};

const char* Col(sqlite3_stmt* s, int c) {
  const unsigned char* t = sqlite3_column_text(s, c);
  return (t != nullptr) ? reinterpret_cast<const char*>(t) : "";
}

}  // namespace

Extractor::Extractor(std::string db_path, const Politeness& politeness,
                     const std::atomic<bool>& sweeping, EnqueueFn enqueue)
    : db_path_(std::move(db_path)),
      politeness_(politeness),
      sweeping_(sweeping),
      enqueue_(std::move(enqueue)) {}

Extractor::~Extractor() {
  Stop();
  if (thread_.joinable()) thread_.join();
}

bool Extractor::Start(std::string* err) {
  db_ = Database::Open(db_path_, Access::ReadOnly, err);
  if (!db_) return false;
  thread_ = std::thread([this] { Run(); });
  return true;
}

void Extractor::Stop() {
  stop_.store(true);
  {
    std::lock_guard<std::mutex> lk(mtx_);
    notified_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void Extractor::Notify() {
  {
    std::lock_guard<std::mutex> lk(mtx_);
    notified_ = true;
  }
  cv_.notify_all();
}

void Extractor::WaitForWork(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lk(mtx_);
  cv_.wait_for(lk, timeout, [this] { return stop_.load() || notified_; });
  notified_ = false;
}

void Extractor::Run() {
  // Below the writer is tempting, but BACKGROUND QoS throttles disk I/O on
  // macOS and would starve a file-reading loop; match the writer's UTILITY.
  pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);

  while (!stop_.load()) {
    // Park (no DB, no disk) while a full crawl holds the writer thread or the
    // machine is under thermal pressure. The writer notifies us when a crawl
    // ends; otherwise the short poll resumes us promptly.
    while (!stop_.load() &&
           (sweeping_.load() || politeness_.ShouldPauseSweep())) {
      WaitForWork(kThermalPark);
    }
    if (stop_.load()) break;

    // Pull a batch of stale files. On battery, defer the older backlog: only
    // files encountered (indexed_at) within the last week stay current.
    const bool on_battery = politeness_.OnBattery();
    const long long now = static_cast<long long>(::time(nullptr));
    std::string sql =
        "SELECT id,path,ext,kind,fingerprint FROM files "
        "WHERE missing=0 AND needs_extract=1";
    if (on_battery) sql += " AND indexed_at >= ?1";
    sql += " ORDER BY indexed_at DESC LIMIT ?2";

    std::string err;
    std::vector<Row> rows;
    {
      Stmt s(db_->handle(), sql, &err);
      if (!s) {
        std::fprintf(stderr, "extractor: prepare failed: %s\n", err.c_str());
        WaitForWork(kIdlePoll);
        continue;
      }
      if (on_battery) {
        sqlite3_bind_int64(s.get(), 1, now - kBatteryFreshSecs);
        sqlite3_bind_int(s.get(), 2, kBatch);
      } else {
        sqlite3_bind_int(s.get(), 1, kBatch);
      }
      while (sqlite3_step(s.get()) == SQLITE_ROW) {
        Row r;
        r.id = sqlite3_column_int64(s.get(), 0);
        r.path = Col(s.get(), 1);
        r.ext = Col(s.get(), 2);
        r.kind = Col(s.get(), 3);
        r.fingerprint = Col(s.get(), 4);
        rows.push_back(std::move(r));
      }
    }

    if (rows.empty()) {
      // Nothing to do (or the whole backlog is deferred on battery). Re-poll
      // after a wait -- plugging in / new work both arrive as a Notify().
      WaitForWork(kIdlePoll);
      continue;
    }

    for (const Row& r : rows) {
      if (stop_.load()) return;
      auto res = std::make_shared<ExtractResult>();
      res->file_id = r.id;
      res->fingerprint = r.fingerprint;
      // Resolve the download date here (off the writer/crawl thread): the
      // quarantine-xattr fast path plus a Spotlight lookup. Done for every file
      // regardless of type, so a download of any kind gets dated. 0 = none.
      long long dl = 0;
      if (ReadDownloadDate(r.path, &dl)) res->downloaded_at = dl;
      // Dispatch by type. Deferred/unsupported types (rtf, office) and any
      // failure leave the payload empty; the file is still stamped done by the
      // writer's guarded clear, so it never loops.
      if (r.ext == "txt" || r.ext == "md" || r.ext == "csv") {
        ExtractPlainText(r.path, *res);
      } else if (r.ext == "pdf") {
        ExtractPdfText(r.path, *res);
      } else if (r.kind == "image" || r.kind == "audio" || r.kind == "video") {
        ExtractMediaTags(r.path, r.kind, *res);
      }
      enqueue_(std::move(res));
    }

    // Lock-step: wait for the writer to apply this batch (clearing the flag)
    // before pulling again, so we don't re-extract not-yet-applied rows.
    WaitForWork(kApplyWait);
  }
}

}  // namespace macassist
