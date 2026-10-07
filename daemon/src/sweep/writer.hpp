#pragma once
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sweep/sweep_status.hpp"

namespace macassist {

class Database;

// Owns the single read-write connection and the long-lived thread that is
// the only mutator of the index. The thread runs the initial crawl as a
// batch job, then parks (blocked, zero CPU) until Stop(). Later bricks wake
// it to drain a command queue fed by the FSEvents watcher and extraction
// workers, and to run periodic reconciliation -- all on this one thread, so
// the write connection is never touched concurrently (see PLAN.md section 5).
class Writer {
 public:
  Writer(std::string db_path, std::vector<std::string> roots);
  ~Writer();  // signals stop and joins the thread

  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  // Opens the write connection and runs migrations synchronously (so a
  // reader may open against the finished schema), then launches the thread.
  // Returns false and sets *err on failure.
  bool Start(std::string* err);

  // Asks the thread to finish the current sweep and exit (idempotent).
  void Stop();

  // Live progress of the current/last sweep, for the status IPC.
  const SweepStatus& status() const { return status_; }

 private:
  void Run();  // thread body

  std::string db_path_;
  std::vector<std::string> roots_;
  std::unique_ptr<Database> db_;  // write connection; thread-owned after Start
  SweepStatus status_;
  std::atomic<bool> stop_{false};
  std::mutex mtx_;
  std::condition_variable cv_;
  std::thread thread_;
};

}  // namespace macassist
