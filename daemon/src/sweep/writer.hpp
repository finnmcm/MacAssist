#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sweep/politeness.hpp"
#include "sweep/sweep_status.hpp"

namespace macassist {

class Database;
class Indexer;
class FsWatcher;

// A mutation the writer thread must apply, produced off-thread by the
// FsWatcher and consumed by the writer. The full crawl does NOT go through
// here -- it runs directly on the writer thread as a batch (PLAN.md section 5).
struct WriteCommand {
  enum class Kind { kExaminePath, kFullRescan };
  Kind kind;
  std::string path;       // kExaminePath: path to re-stat and reconcile
  uint64_t event_id = 0;  // FSEvents id, for replay bookkeeping
};

// Owns the single read-write connection and the long-lived thread that is the
// only mutator of the index. The thread runs the initial crawl, then drains
// FSEvents-driven commands and, once idle for a full reconcile interval, runs a
// reconciliation crawl (the FSEvents-drop / offline backstop) -- all on this
// one thread, so the write connection is never touched concurrently.
class Writer {
 public:
  Writer(std::string db_path, std::vector<std::string> roots,
         std::chrono::seconds reconcile_interval);
  ~Writer();  // signals stop and joins the thread

  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  // Opens the write connection and runs migrations synchronously (so a reader
  // may open against the finished schema), launches the thread, and starts the
  // FSEvents watcher. Returns false and sets *err if the connection fails.
  bool Start(std::string* err);

  // Stops the watcher and asks the thread to finish and exit (idempotent).
  void Stop();

  // Live progress of the current/last sweep, for the status IPC.
  const SweepStatus& status() const { return status_; }

 private:
  void Run();  // thread body
  void RunFullCrawl(Indexer& idx, const char* reason);
  void ApplyBatch(Indexer& idx, const std::deque<WriteCommand>& batch);
  long long RootIdForPath(Indexer& idx, const std::string& path);
  void Enqueue(WriteCommand cmd);

  std::string db_path_;
  std::vector<std::string> roots_;  // canonicalized to match FSEvents paths
  std::chrono::seconds reconcile_interval_;
  std::unique_ptr<Database> db_;  // write connection; thread-owned after Start
  std::unique_ptr<FsWatcher> watcher_;
  Politeness politeness_;
  SweepStatus status_;

  std::atomic<bool> stop_{false};
  std::mutex mtx_;
  std::condition_variable cv_;
  std::deque<WriteCommand> queue_;  // guarded by mtx_
  std::chrono::steady_clock::time_point last_full_pass_{};  // writer-thread only

  std::thread thread_;
};

}  // namespace macassist
