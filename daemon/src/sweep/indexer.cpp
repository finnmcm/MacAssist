#include "sweep/indexer.hpp"

#include <sqlite3.h>
#include <sys/stat.h>

#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>

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
  sel_ = Stmt(db_.handle(), "SELECT id, fingerprint FROM files WHERE path=?1",
              &err);
  ins_ = Stmt(db_.handle(),
              "INSERT INTO files"
              "(root_id,path,name,ext,kind,size,created_at,modified_at,"
              " indexed_at,fingerprint,missing,seen_gen)"
              " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,0,?11)",
              &err);
  insft_ = Stmt(db_.handle(),
                "INSERT INTO file_text(rowid,name,path_tokens,content,tags)"
                " VALUES(?1,?2,?3,'','')",
                &err);
  upd_ = Stmt(db_.handle(),
              "UPDATE files SET size=?1,created_at=?2,modified_at=?3,"
              " indexed_at=?4,fingerprint=?5,missing=0,seen_gen=?6 WHERE id=?7",
              &err);
  touch_ = Stmt(db_.handle(),
                "UPDATE files SET missing=0,seen_gen=?1 WHERE id=?2", &err);
  tomb_path_ =
      Stmt(db_.handle(), "UPDATE files SET missing=1 WHERE path=?1", &err);
  tomb_stale_ = Stmt(db_.handle(),
                     "UPDATE files SET missing=1 WHERE seen_gen < ?1 AND "
                     "missing=0",
                     &err);
  ins_root_ =
      Stmt(db_.handle(), "INSERT OR IGNORE INTO roots(path) VALUES(?1)", &err);
  sel_root_ = Stmt(db_.handle(), "SELECT id FROM roots WHERE path=?1", &err);
  valid_ = sel_ && ins_ && insft_ && upd_ && touch_ && tomb_path_ &&
           tomb_stale_ && ins_root_ && sel_root_;
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

  const std::string ext = LowerExt(path);
  const std::string kind = KindForExtension(ext);
  if (kind.empty()) return Result::kSkipped;  // exists, but not an indexed type

  const std::string name = fs::path(path).filename().string();
  const long long size = static_cast<long long>(st.st_size);
  const long long mtime = static_cast<long long>(st.st_mtimespec.tv_sec);
  const long long ctime = static_cast<long long>(st.st_birthtimespec.tv_sec);
  const long long now = static_cast<long long>(::time(nullptr));
  const std::string fp = std::to_string(mtime) + ":" + std::to_string(size);

  sqlite3_reset(sel_.get());
  sqlite3_bind_text(sel_.get(), 1, path.c_str(), -1, SQLITE_TRANSIENT);
  long long existing_id = 0;
  std::string existing_fp;
  if (sqlite3_step(sel_.get()) == SQLITE_ROW) {
    existing_id = sqlite3_column_int64(sel_.get(), 0);
    const unsigned char* t = sqlite3_column_text(sel_.get(), 1);
    if (t != nullptr) existing_fp = reinterpret_cast<const char*>(t);
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
    sqlite3_step(s);
    return Result::kUpdated;
  }

  // Unchanged: just mark it seen this generation and clear any tombstone.
  sqlite3_stmt* s = touch_.get();
  sqlite3_reset(s);
  sqlite3_bind_int64(s, 1, gen);
  sqlite3_bind_int64(s, 2, existing_id);
  sqlite3_step(s);
  return Result::kUnchanged;
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
