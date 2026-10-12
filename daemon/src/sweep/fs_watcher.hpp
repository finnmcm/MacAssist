#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace macassist {

// Watches a set of roots via FSEvents and reports changes through callbacks,
// delivered on a private serial dispatch queue (GCD owns the kernel-blocking
// wait -- no busy-waiting, no hand-rolled run loop). Semantics are idempotent
// "a path changed, re-examine it": flags are not interpreted beyond detecting
// drops, because FSEvents coalesces and a rename fires on both old and new
// paths. The owner re-stats each reported path and reconciles it.
class FsWatcher {
 public:
  // sinceWhen sentinel meaning "only events from now on" (first run).
  static const uint64_t kSinceNow;

  // on_path(path, eventId): a watched path changed -- re-stat and reconcile.
  // on_rescan(): FSEvents dropped detail -- a full rescan is needed.
  using PathSink = std::function<void(const std::string&, uint64_t)>;
  using RescanSink = std::function<void()>;

  FsWatcher(std::vector<std::string> roots, PathSink on_path,
            RescanSink on_rescan);
  ~FsWatcher();

  FsWatcher(const FsWatcher&) = delete;
  FsWatcher& operator=(const FsWatcher&) = delete;

  // Begins delivering events after `since_when` (kSinceNow on first run).
  // Returns false if the stream could not be created or started.
  bool Start(uint64_t since_when);
  void Stop();  // idempotent

  // Defined in the .mm; public only so the C FSEvents callback can reach it.
  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace macassist
