#pragma once
#include <string>
#include <vector>

namespace macassist {

class Database;

struct CrawlStats {
  long long scanned = 0;  // regular files visited
  long long indexed = 0;  // files of an indexed type actually stored
};

// Rebuilds the index from scratch: clears existing rows, then walks each
// root recursively and stores metadata (path, name, size, timestamps) for
// every known file type. No content extraction yet -- that is a later
// brick. Excluded / hidden directories are pruned during the walk.
CrawlStats CrawlRebuild(Database& db, const std::vector<std::string>& roots);

}  // namespace macassist
