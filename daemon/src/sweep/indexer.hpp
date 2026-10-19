#pragma once
#include <string>

#include "index/database.hpp"  // Database, Stmt

struct stat;  // <sys/stat.h>, included in the .cpp; passed here by reference

namespace macassist {

struct ExtractResult;

// Owns the prepared statements and the per-file upsert / tombstone logic
// shared by the full crawl and the FSEvents handler. Lives on and is used
// only by the writer thread, so it is deliberately not thread-safe.
// Construct after the write connection is open and check valid() before use.
class Indexer {
 public:
  explicit Indexer(Database& db);

  Indexer(const Indexer&) = delete;
  Indexer& operator=(const Indexer&) = delete;

  bool valid() const { return valid_; }

  enum class Result { kMissing, kSkipped, kInserted, kUpdated, kUnchanged };

  // Stat `path`; if it is an indexed-type regular file, insert or update its
  // row (fingerprint decides) stamped with seen_gen=gen. root_id is used only
  // when inserting a new row. kMissing means the path is gone (the caller may
  // tombstone it); kSkipped means it exists but is not an indexed file.
  Result UpsertFile(const std::string& path, long long root_id, long long gen);

  // Like UpsertFile, but for a directory: inserts/updates a kind="folder" row
  // (name/path searchable, needs_extract=0 -- folders have no body to extract).
  // kSkipped if `path` is not a directory.
  Result UpsertDir(const std::string& path, long long root_id, long long gen);

  // Dispatch on what `path` actually is (one stat): a directory is upserted as
  // a folder only when index_folders is set (else kSkipped); a regular file is
  // upserted as a file. For the FSEvents handler, which gets a bare path and
  // does not know file-vs-dir up front.
  Result UpsertPath(const std::string& path, long long root_id, long long gen,
                    bool index_folders);

  // Apply an extraction result: write content/tags into the FTS row, replace
  // the file's file_meta rows, and clear needs_extract -- but only if the
  // file's fingerprint still matches the one the extractor saw (otherwise the
  // file changed mid-extraction and stays flagged for a re-run).
  void ApplyExtraction(const ExtractResult& r);

  // Mark one path missing (a delete / move-away observed live).
  void TombstoneByPath(const std::string& path);

  // Mark every row left at an older generation missing (end of a full pass).
  void TombstoneStale(long long gen);

  // Ensure a root row exists; returns its id (0 on error).
  long long EnsureRoot(const std::string& root);

  // Last completed crawl generation, stored in meta (0 if never).
  long long ReadGen();
  void WriteGen(long long gen);

 private:
  // Shared insert/update body for a file (is_dir=false) or folder (is_dir=true)
  // whose stat the caller already took. Chooses kind + needs_extract from
  // is_dir, then runs the sel/insert/update/touch path.
  Result UpsertStat(const std::string& path, long long root_id, long long gen,
                    const struct stat& st, bool is_dir);

  Database& db_;
  bool valid_ = false;
  Stmt sel_, ins_, insft_, upd_, touch_, set_tags_, set_dl_, tomb_path_,
      tomb_stale_;
  Stmt ins_root_, sel_root_;
  Stmt updft_, del_meta_, ins_meta_, clear_extract_;
};

}  // namespace macassist
