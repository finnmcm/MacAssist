#pragma once
#include <string>

#include "index/database.hpp"  // Database, Stmt

namespace macassist {

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
  Database& db_;
  bool valid_ = false;
  Stmt sel_, ins_, insft_, upd_, touch_, tomb_path_, tomb_stale_;
  Stmt ins_root_, sel_root_;
};

}  // namespace macassist
