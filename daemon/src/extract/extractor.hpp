#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace macassist {

class Database;
class Politeness;
struct ExtractResult;

// Pulls files still flagged needs_extract on its own read-only connection,
// extracts their content/tags/meta off the writer thread, and funnels the
// results back through an enqueue callback (the writer applies them to the one
// read-write connection). See BRICK_5.md / PLAN.md section 5.
//
// Lock-step: exactly one batch is in flight. After enqueuing a batch the
// extractor blocks until the writer Notify()s that it was applied -- so it
// never re-selects rows the writer hasn't cleared yet, and the write-command
// queue holds at most one batch of (<=100 KB) payloads. Thermal pressure parks
// the loop; on battery it defers the older backlog (see the pull query).
class Extractor {
 public:
  using EnqueueFn = std::function<void(std::shared_ptr<ExtractResult>)>;

  // `db_path` is opened ReadOnly on the extractor's own thread. `politeness`,
  // `sweeping`, and the enqueue sink are owned by the caller (the Writer) and
  // must outlive this object. While `sweeping` is set (a full crawl is running
  // on the writer thread), the extractor parks: the writer can't drain applied
  // results mid-crawl, so running would risk re-extracting un-applied rows.
  Extractor(std::string db_path, const Politeness& politeness,
            const std::atomic<bool>& sweeping, EnqueueFn enqueue);
  ~Extractor();

  Extractor(const Extractor&) = delete;
  Extractor& operator=(const Extractor&) = delete;

  // Opens the read-only connection and launches the thread. The schema must
  // already exist (the writer migrates first). Returns false / sets *err on
  // failure to open; a failed start just means no extraction runs.
  bool Start(std::string* err);

  // Signals the thread to finish and joins it (idempotent). Must be called
  // before the writer stops draining, so no more results are enqueued.
  void Stop();

  // Writer -> extractor wakeup: a batch was applied or new work may exist.
  void Notify();

 private:
  void Run();
  // Blocks until notified, stopped, or `timeout` elapses; clears the flag.
  void WaitForWork(std::chrono::milliseconds timeout);

  std::string db_path_;
  const Politeness& politeness_;
  const std::atomic<bool>& sweeping_;
  EnqueueFn enqueue_;
  std::unique_ptr<Database> db_;  // read-only; thread-owned after Start

  std::atomic<bool> stop_{false};
  std::mutex mtx_;
  std::condition_variable cv_;
  bool notified_ = false;  // guarded by mtx_

  std::thread thread_;
};

}  // namespace macassist
