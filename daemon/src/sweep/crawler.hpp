#pragma once
#include <atomic>
#include <string>
#include <vector>

namespace macassist {

class Database;
struct SweepStatus;

// Brings the index in line with the filesystem in one pass. For each root it
// walks the tree and, for every file of a known type: inserts new files,
// updates changed ones (detected by fingerprint), and simply re-stamps
// unchanged ones. Files not seen this pass are tombstoned (missing=1) via a
// monotonic generation counter -- that is how deletes and moves-away are
// caught. Updates `status` live and returns early if `stop` becomes set.
// Replaces the Phase 1 destroy-and-rebuild crawl.
void CrawlIncremental(Database& db, const std::vector<std::string>& roots,
                      SweepStatus& status, const std::atomic<bool>& stop);

}  // namespace macassist
