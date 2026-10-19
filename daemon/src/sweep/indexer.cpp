#include "sweep/indexer.hpp"

#include <sqlite3.h>
#include <sys/stat.h>

#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>

#include "extract/finder_tags.hpp"
#include "extract/text_extractors.hpp"
#include "index/file_kind.hpp"

namespace fs = std::filesystem;

namespace macassist {
namespace {

std::string LowerExt(const std::string& path) {
  std::string e = fs::path(path).extension().string();
  if (!e.empty() && e[0] == '.') e.erase(0, 1);
  for (char& c : e) c = static_cast<char>(std::tolower((unsigned char)c));
  return e;
}

// Full path with non-alphanumeric runs replaced by spaces, so directory
// names become searchable FTS tokens (e.g. ".../Job Apps/resume.pdf").
std::string PathTokens(const std::string& path) {
  std::string out;
  out.reserve(path.size());
  for (char c : path) out.push_back(std::isalnum((unsigned char)c) ? c : ' ');
  return out;
}

}  // namespace

Indexer::Indexer(Database& db) : db_(db) {
  std::string err;
  sel_ = Stmt(db_.handle(),
              "SELECT id, fingerprint, tags_json FROM files WHERE path=?1",
              &err);
  ins_ = Stmt(db_.handle(),
              "INSERT INTO files"
              "(root_id,path,name,ext,kind,size,created_at,modified_at,"
              " indexed_at,fingerprint,missing,seen_gen,tags_json,needs_extract)"
              " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,0,?11,?12,?13)",
              &err);
  insft_ = Stmt(db_.handle(),
                "INSERT INTO file_text(rowid,name,path_tokens,content,tags)"
                " VALUES(?1,?2,?3,'','')",
                &err);
  // A fingerprint change re-flags the file for extraction (needs_extract bound
  // to 1 for files); the extractor will refresh its content/tags/meta. Folders
  // bind 0 -- they have no body to extract.
  upd_ = Stmt(db_.handle(),
              "UPDATE files SET size=?1,created_at=?2,modified_at=?3,"
              " indexed_at=?4,fingerprint=?5,missing=0,seen_gen=?6,"
              " needs_extract=?9,tags_json=?8 WHERE id=?7",
              &err);
  touch_ = Stmt(db_.handle(),
                "UPDATE files SET missing=0,seen_gen=?1 WHERE id=?2", &err);
  // Finder tags change without touching mtime/size, so the "unchanged" path
  // still refreshes them -- but only when they actually differ, so a quiet
  // reconcile pass doesn't rewrite every row.
  set_tags_ =
      Stmt(db_.handle(), "UPDATE files SET tags_json=?1 WHERE id=?2", &err);
  // Download date, written from an extraction payload (resolved off the writer
  // thread). Immutable once known; ApplyExtraction only writes a resolved
  // value, so this never clobbers a recorded date back to NULL.
  set_dl_ = Stmt(db_.handle(),
                 "UPDATE files SET downloaded_at=?1 WHERE id=?2", &err);
  tomb_path_ =
      Stmt(db_.handle(), "UPDATE files SET missing=1 WHERE path=?1", &err);
  tomb_stale_ = Stmt(db_.handle(),
                     "UPDATE files SET missing=1 WHERE seen_gen < ?1 AND "
                     "missing=0",
                     &err);
  ins_root_ =
      Stmt(db_.handle(), "INSERT OR IGNORE INTO roots(path) VALUES(?1)", &err);
  sel_root_ = Stmt(db_.handle(), "SELECT id FROM roots WHERE path=?1", &err);
  // Extraction apply path (brick 5).
  updft_ = Stmt(db_.handle(),
                "UPDATE file_text SET content=?1,tags=?2 WHERE rowid=?3", &err);
  del_meta_ =
      Stmt(db_.handle(), "DELETE FROM file_meta WHERE file_id=?1", &err);
  ins_meta_ = Stmt(db_.handle(),
                   "INSERT INTO file_meta(file_id,key,value) VALUES(?1,?2,?3)",
                   &err);
  // Guarded clear: only mark extracted if the fingerprint still matches what
  // the extractor saw, so a file edited mid-extraction stays flagged.
  clear_extract_ = Stmt(db_.handle(),
                        "UPDATE files SET needs_extract=0 "
                        "WHERE id=?1 AND fingerprint=?2",
                        &err);
  valid_ = sel_ && ins_ && insft_ && upd_ && touch_ && set_tags_ && set_dl_ &&
           tomb_path_ && tomb_stale_ && ins_root_ && sel_root_ && updft_ &&
           del_meta_ && ins_meta_ && clear_extract_;
}

long long Indexer::EnsureRoot(const std::string& root) {
  sqlite3_reset(ins_root_.get());
  sqlite3_bind_text(ins_root_.get(), 1, root.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(ins_root_.get());
  sqlite3_reset(sel_root_.get());
  sqlite3_bind_text(sel_root_.get(), 1, root.c_str(), -1, SQLITE_TRANSIENT);
  long long id = 0;
  if (sqlite3_step(sel_root_.get()) == SQLITE_ROW) {
    id = sqlite3_column_int64(sel_root_.get(), 0);
  }
  sqlite3_reset(sel_root_.get());
  return id;
}

Indexer::Result Indexer::UpsertFile(const std::string& path, long long root_id,
                                    long long gen) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0) return Result::kMissing;
  if (!S_ISREG(st.st_mode)) return Result::kSkipped;
  return UpsertStat(path, root_id, gen, st, /*is_dir=*/false);
}

Indexer::Result Indexer::UpsertDir(const std::string& path, long long root_id,
                                   long long gen) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0) return Result::kMissing;
  if (!S_ISDIR(st.st_mode)) return Result::kSkipped;
  return UpsertStat(path, root_id, gen, st, /*is_dir=*/true);
}

Indexer::Result Indexer::UpsertPath(const std::string& path, long long root_id,
                                    long long gen, bool index_folders) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0) return Result::kMissing;
  if (S_ISDIR(st.st_mode)) {
    if (!index_folders) return Result::kSkipped;
    return UpsertStat(path, root_id, gen, st, /*is_dir=*/true);
  }
  if (!S_ISREG(st.st_mode)) return Result::kSkipped;
  return UpsertStat(path, root_id, gen, st, /*is_dir=*/false);
}

Indexer::Result Indexer::UpsertStat(const std::string& path, long long root_id,
                                    long long gen, const struct stat& st,
                                    bool is_dir) {
  std::string ext;
  std::string kind;
  // Files flag themselves for extraction; folders have no body, so they skip
  // it (and stay out of the extractor's pending backlog).
  const int needs_extract = is_dir ? 0 : 1;
  if (is_dir) {
    kind = "folder";
  } else {
    ext = LowerExt(path);
    kind = KindForExtension(ext);
    if (kind.empty()) return Result::kSkipped;  // exists, not an indexed type
  }

  const std::string name = fs::path(path).filename().string();
  const long long size = static_cast<long long>(st.st_size);
  const long long mtime = static_cast<long long>(st.st_mtimespec.tv_sec);
  const long long ctime = static_cast<long long>(st.st_birthtimespec.tv_sec);
  const long long now = static_cast<long long>(::time(nullptr));
  const std::string fp = std::to_string(mtime) + ":" + std::to_string(size);
  const std::string tags = ReadFinderTagsJson(path);

  sqlite3_reset(sel_.get());
  sqlite3_bind_text(sel_.get(), 1, path.c_str(), -1, SQLITE_TRANSIENT);
  long long existing_id = 0;
  std::string existing_fp;
  std::string existing_tags;
  if (sqlite3_step(sel_.get()) == SQLITE_ROW) {
    existing_id = sqlite3_column_int64(sel_.get(), 0);
    const unsigned char* t = sqlite3_column_text(sel_.get(), 1);
    if (t != nullptr) existing_fp = reinterpret_cast<const char*>(t);
    const unsigned char* tg = sqlite3_column_text(sel_.get(), 2);
    if (tg != nullptr) existing_tags = reinterpret_cast<const char*>(tg);
  }
  sqlite3_reset(sel_.get());  // release the read cursor before any commit

  if (existing_id == 0) {
    sqlite3_stmt* s = ins_.get();
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, root_id);
    sqlite3_bind_text(s, 2, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 4, ext.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 5, kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 6, size);
    sqlite3_bind_int64(s, 7, ctime);
    sqlite3_bind_int64(s, 8, mtime);
    sqlite3_bind_int64(s, 9, now);
    sqlite3_bind_text(s, 10, fp.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 11, gen);
    sqlite3_bind_text(s, 12, tags.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 13, needs_extract);
    // downloaded_at is left NULL here and resolved later by the extractor
    // (off the writer thread); see ApplyExtraction.
    if (sqlite3_step(s) != SQLITE_DONE) return Result::kSkipped;
    const long long id = sqlite3_last_insert_rowid(db_.handle());

    const std::string toks = PathTokens(path);
    sqlite3_stmt* f = insft_.get();
    sqlite3_reset(f);
    sqlite3_bind_int64(f, 1, id);
    sqlite3_bind_text(f, 2, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(f, 3, toks.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(f) != SQLITE_DONE) return Result::kSkipped;
    return Result::kInserted;
  }

  if (fp != existing_fp) {
    sqlite3_stmt* s = upd_.get();
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, size);
    sqlite3_bind_int64(s, 2, ctime);
    sqlite3_bind_int64(s, 3, mtime);
    sqlite3_bind_int64(s, 4, now);
    sqlite3_bind_text(s, 5, fp.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 6, gen);
    sqlite3_bind_int64(s, 7, existing_id);
    sqlite3_bind_text(s, 8, tags.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 9, needs_extract);
    sqlite3_step(s);
    // For files, needs_extract is re-set to 1 by upd_, so the extractor
    // re-resolves content/downloaded_at on a change (e.g. a re-download);
    // folders bind 0.
    return Result::kUpdated;
  }

  // Unchanged body: just mark it seen this generation and clear any tombstone.
  sqlite3_stmt* s = touch_.get();
  sqlite3_reset(s);
  sqlite3_bind_int64(s, 1, gen);
  sqlite3_bind_int64(s, 2, existing_id);
  sqlite3_step(s);

  // ...but Finder tags may still have changed under an unchanged fingerprint.
  if (tags != existing_tags) {
    sqlite3_stmt* t = set_tags_.get();
    sqlite3_reset(t);
    sqlite3_bind_text(t, 1, tags.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(t, 2, existing_id);
    sqlite3_step(t);
  }
  return Result::kUnchanged;
}

void Indexer::ApplyExtraction(const ExtractResult& r) {
  // Body text + flattened tags into the FTS row (name/path_tokens preserved).
  sqlite3_stmt* ft = updft_.get();
  sqlite3_reset(ft);
  sqlite3_bind_text(ft, 1, r.content.c_str(), static_cast<int>(r.content.size()),
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(ft, 2, r.tags.c_str(), static_cast<int>(r.tags.size()),
                    SQLITE_TRANSIENT);
  sqlite3_bind_int64(ft, 3, r.file_id);
  sqlite3_step(ft);

  // Replace the typed metadata rows for this file.
  sqlite3_reset(del_meta_.get());
  sqlite3_bind_int64(del_meta_.get(), 1, r.file_id);
  sqlite3_step(del_meta_.get());
  for (const auto& kv : r.meta) {
    sqlite3_stmt* m = ins_meta_.get();
    sqlite3_reset(m);
    sqlite3_bind_int64(m, 1, r.file_id);
    sqlite3_bind_text(m, 2, kv.first.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(m, 3, kv.second.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(m);
  }

  // Persist the download date the extractor resolved. Only when it got a value
  // (>0): a file with no download signal, or one whose volatile quarantine
  // xattr has since vanished, must not reset an already-recorded date to NULL.
  if (r.downloaded_at > 0) {
    sqlite3_stmt* d = set_dl_.get();
    sqlite3_reset(d);
    sqlite3_bind_int64(d, 1, r.downloaded_at);
    sqlite3_bind_int64(d, 2, r.file_id);
    sqlite3_step(d);
  }

  // Clear the flag only if the file hasn't changed since the extractor read it.
  sqlite3_stmt* c = clear_extract_.get();
  sqlite3_reset(c);
  sqlite3_bind_int64(c, 1, r.file_id);
  sqlite3_bind_text(c, 2, r.fingerprint.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(c);
}

void Indexer::TombstoneByPath(const std::string& path) {
  sqlite3_reset(tomb_path_.get());
  sqlite3_bind_text(tomb_path_.get(), 1, path.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(tomb_path_.get());
}

void Indexer::TombstoneStale(long long gen) {
  sqlite3_reset(tomb_stale_.get());
  sqlite3_bind_int64(tomb_stale_.get(), 1, gen);
  sqlite3_step(tomb_stale_.get());
}

long long Indexer::ReadGen() {
  const std::string v = db_.GetMeta("crawl_gen");
  return v.empty() ? 0 : std::atoll(v.c_str());
}

void Indexer::WriteGen(long long gen) {
  std::string err;
  db_.SetMeta("crawl_gen", std::to_string(gen), &err);
}

}  // namespace macassist
