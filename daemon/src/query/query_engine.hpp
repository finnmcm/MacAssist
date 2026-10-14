#pragma once
#include <set>
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
  std::string tags_json;  // Finder color tags, [{"name","color"}] or empty
};

// Filter-toolbar selections ANDed onto a query. Same-type values are ORed
// (kind IN (...), tag color IN (...)); the groups AND together and AND with
// the text match. All default-empty, so an empty QueryFilters reproduces the
// pre-toolbar behavior.
struct QueryFilters {
  std::set<std::string> kinds;    // canonical file kinds (image|audio|video|document)
  std::set<int> tag_colors;       // Finder label indices (1..7)
  bool downloaded_only = false;   // files with a resolved downloaded_at

  bool empty() const {
    return kinds.empty() && tag_colors.empty() && !downloaded_only;
  }
};

// Runs a filename/path keyword search over the FTS index for `text`, narrowed
// by `filters`, and returns up to `limit` hits best match first. Free text is
// sanitized into a safe FTS5 prefix query. With no usable text but at least
// one filter, returns the matching files ordered by recency (downloaded_at
// DESC when the downloaded filter is active, else modified_at DESC). Returns
// empty when there is neither searchable text nor any filter.
std::vector<FileHit> SearchFiles(Database& db, const std::string& text,
                                 int limit, const QueryFilters& filters = {});

}  // namespace macassist
