#include "sweep/fs_watcher.hpp"

#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>

#include <cstdio>
#include <utility>

namespace macassist {

const uint64_t FsWatcher::kSinceNow = kFSEventStreamEventIdSinceNow;

struct FsWatcher::Impl {
  std::vector<std::string> roots;
  PathSink on_path;
  RescanSink on_rescan;
  FSEventStreamRef stream = nullptr;
  dispatch_queue_t queue = nullptr;
};

namespace {

void Callback(ConstFSEventStreamRef /*stream*/, void* info, size_t count,
              void* paths, const FSEventStreamEventFlags flags[],
              const FSEventStreamEventId ids[]) {
  auto* impl = static_cast<FsWatcher::Impl*>(info);
  const char** p = static_cast<const char**>(paths);
  const FSEventStreamEventFlags kDrop =
      kFSEventStreamEventFlagMustScanSubDirs |
      kFSEventStreamEventFlagUserDropped | kFSEventStreamEventFlagKernelDropped;
  for (size_t i = 0; i < count; ++i) {
    if (flags[i] & kDrop) {
      if (impl->on_rescan) impl->on_rescan();  // lost detail -> full rescan
      continue;
    }
    if (impl->on_path) impl->on_path(std::string(p[i]), ids[i]);
  }
}

}  // namespace

FsWatcher::FsWatcher(std::vector<std::string> roots, PathSink on_path,
                     RescanSink on_rescan)
    : impl_(std::make_unique<Impl>()) {
  impl_->roots = std::move(roots);
  impl_->on_path = std::move(on_path);
  impl_->on_rescan = std::move(on_rescan);
}

FsWatcher::~FsWatcher() { Stop(); }

bool FsWatcher::Start(uint64_t since_when) {
  if (impl_->stream != nullptr) return true;  // already running
  if (impl_->roots.empty()) return false;

  CFMutableArrayRef paths = CFArrayCreateMutable(nullptr, impl_->roots.size(),
                                                 &kCFTypeArrayCallBacks);
  for (const auto& r : impl_->roots) {
    CFStringRef s =
        CFStringCreateWithCString(nullptr, r.c_str(), kCFStringEncodingUTF8);
    CFArrayAppendValue(paths, s);
    CFRelease(s);
  }

  FSEventStreamContext ctx{};
  ctx.info = impl_.get();

  // FileEvents: per-file granularity. NoDefer: deliver the first event of an
  // idle period promptly, then coalesce bursts within the latency window.
  impl_->stream = FSEventStreamCreate(
      nullptr, &Callback, &ctx, paths,
      static_cast<FSEventStreamEventId>(since_when), /*latency=*/2.0,
      kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);
  CFRelease(paths);
  if (impl_->stream == nullptr) return false;

  impl_->queue = dispatch_queue_create("com.finn.macassist.fsevents",
                                        DISPATCH_QUEUE_SERIAL);
  FSEventStreamSetDispatchQueue(impl_->stream, impl_->queue);
  if (!FSEventStreamStart(impl_->stream)) {
    FSEventStreamInvalidate(impl_->stream);
    FSEventStreamRelease(impl_->stream);
    impl_->stream = nullptr;
    impl_->queue = nullptr;  // ARC releases the dispatch queue
    return false;
  }
  return true;
}

void FsWatcher::Stop() {
  if (impl_->stream != nullptr) {
    FSEventStreamStop(impl_->stream);
    FSEventStreamInvalidate(impl_->stream);  // unschedules from the queue
    FSEventStreamRelease(impl_->stream);
    impl_->stream = nullptr;
  }
  impl_->queue = nullptr;  // ARC releases the dispatch queue
}

}  // namespace macassist
