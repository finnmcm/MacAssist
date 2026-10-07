#include "sweep/writer.hpp"

#include <cstdio>

#include "index/database.hpp"
#include "sweep/crawler.hpp"

namespace macassist {

Writer::Writer(std::string db_path, std::vector<std::string> roots)
    : db_path_(std::move(db_path)), roots_(std::move(roots)) {}

Writer::~Writer() {
  Stop();
  if (thread_.joinable()) thread_.join();
}

bool Writer::Start(std::string* err) {
  // Open + migrate on the calling thread so the schema exists before the
  // reader opens. After this returns, the connection is touched only by Run()
  // -- thread construction provides the happens-before, so NOMUTEX is safe.
  db_ = Database::Open(db_path_, Access::ReadWrite, err);
  if (!db_) return false;
  thread_ = std::thread([this] { Run(); });
  return true;
}

void Writer::Stop() {
  stop_.store(true);
  cv_.notify_all();  // wake the park in Run()
}

void Writer::Run() {
  // Initial sweep: a batch crawl run directly on the writer thread.
  status_.sweeping.store(true);
  std::fprintf(stderr, "macassistd: initial sweep over %zu root(s)...\n",
               roots_.size());
  CrawlIncremental(*db_, roots_, status_, stop_);
  status_.sweeping.store(false);
  std::fprintf(stderr, "macassistd: sweep done -- %lld scanned, %lld indexed\n",
               status_.scanned.load(), status_.indexed.load());

  // Park until Stop(). A later brick replaces this wait with a loop that
  // drains the write-command queue (FSEvents / extraction results) and runs
  // periodic reconciliation.
  std::unique_lock<std::mutex> lk(mtx_);
  cv_.wait(lk, [this] { return stop_.load(); });
}

}  // namespace macassist
