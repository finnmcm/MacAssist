#pragma once
#include <atomic>

namespace macassist {

// Live sweep progress: written by the sweeper thread, read by the status
// IPC handler. Plain atomics rather than a lock -- these are independent
// scalars and the readers only ever observe them (see PLAN.md section 5).
struct SweepStatus {
  std::atomic<bool> sweeping{false};
  std::atomic<long long> scanned{0};  // regular files visited this pass
  std::atomic<long long> indexed{0};  // indexed-type files inserted or updated
};

// Live extraction progress (brick 5): written by the writer as it applies
// extraction results, read by the status IPC. The matching "still pending"
// count is read live from the index (Database::CountPending), not kept here.
struct ExtractStatus {
  std::atomic<long long> extracted{0};  // files whose content/tags were applied
};

}  // namespace macassist
