#pragma once
#include <string>
#include <vector>

namespace macassist {

class Database;

struct FileHit {
  long long id = 0;
  std::string path;
  std::string name;
  std::string kind;
  long long modified_at = 0;
  double score = 0.0;  // bm25: lower is a better match
};

// Runs a filename/path keyword search over the FTS index for `text` and
// returns up to `limit` hits, best match first. Free text is sanitized
// into a safe FTS5 prefix query; returns empty if nothing is searchable.
std::vector<FileHit> SearchFiles(Database& db, const std::string& text,
                                 int limit);

}  // namespace macassist
