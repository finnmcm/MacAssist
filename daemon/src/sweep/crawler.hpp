#pragma once
#include <atomic>
#include <string>
#include <vector>

namespace macassist {

class Database;
class Indexer;
struct SweepStatus;

// Brings the index in line with the filesystem in one full pass, driving
// `idx`: new files are inserted, changed ones updated (by fingerprint),
// unchanged ones re-stamped. Files not seen this pass are tombstoned
// (missing=1) via a monotonic generation counter -- that is how deletes and
// moves-away are caught. Updates `status` live and returns early if `stop`
// becomes set (a partial pass skips tombstoning). Commits in batches.
void CrawlIncremental(Indexer& idx, Database& db,
                      const std::vector<std::string>& roots,
                      SweepStatus& status, const std::atomic<bool>& stop);

}  // namespace macassist
