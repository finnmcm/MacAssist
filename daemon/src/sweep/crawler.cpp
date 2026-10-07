#include "sweep/crawler.hpp"

#include <sqlite3.h>
#include <sys/stat.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_set>

#include "index/database.hpp"
#include "index/file_kind.hpp"
#include "sweep/sweep_status.hpp"

namespace fs = std::filesystem;

namespace macassist {
namespace {

std::string LowerExt(const fs::path& p) {
  std::string e = p.extension().string();
  if (!e.empty() && e[0] == '.') e.erase(0, 1);
  for (char& c : e) c = static_cast<char>(std::tolower((unsigned char)c));
  return e;
}

// Directory names we never descend into (plus any hidden dir).
bool IsExcludedDir(const std::string& name) {
  static const std::unordered_set<std::string> kSkip = {
      "node_modules", ".git",   ".svn",   ".hg",    "Library",
      "DerivedData",  ".Trash", ".cache", "Caches", "Applications"};
  if (!name.empty() && name[0] == '.') return true;  // hidden directories
  return kSkip.count(name) > 0;
}

// Full path with non-alphanumeric runs replaced by spaces, so directory
// names become searchable FTS tokens (e.g. ".../Job Apps/resume.pdf").
std::string PathTokens(const std::string& path) {
  std::string out;
  out.reserve(path.size());
  for (char c : path) {
    out.push_back(std::isalnum((unsigned char)c) ? c : ' ');
  }
  return out;
}

// The last completed crawl generation (0 if never). Each pass runs at the
// next generation and stamps every file it sees; rows left behind at an
// older generation are stale and get tombstoned.
long long ReadCrawlGen(Database& db) {
  std::string err;
  Stmt s(db.handle(), "SELECT value FROM meta WHERE key='crawl_gen'", &err);
  if (!s) return 0;
  if (sqlite3_step(s.get()) == SQLITE_ROW) {
    const unsigned char* t = sqlite3_column_text(s.get(), 0);
    if (t != nullptr) return std::atoll(reinterpret_cast<const char*>(t));
  }
  return 0;
}

void WriteCrawlGen(Database& db, long long gen) {
  std::string err;
  Stmt s(db.handle(),
         "INSERT INTO meta(key,value) VALUES('crawl_gen',?1) "
         "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
         &err);
  if (!s) return;
  const std::string v = std::to_string(gen);
  sqlite3_bind_text(s.get(), 1, v.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(s.get());
}

}  // namespace

void CrawlIncremental(Database& db, const std::vector<std::string>& roots,
                      SweepStatus& status, const std::atomic<bool>& stop) {
  std::string err;
  const long long gen = ReadCrawlGen(db) + 1;
  const long long now = static_cast<long long>(::time(nullptr));

  // Statement set for the upsert. Lookup by path keys everything; the four
  // writers cover the new / changed / unchanged cases.
  Stmt sel(db.handle(), "SELECT id, fingerprint FROM files WHERE path=?1", &err);
  Stmt ins(db.handle(),
           "INSERT INTO files"
           "(root_id,path,name,ext,kind,size,created_at,modified_at,"
           " indexed_at,fingerprint,missing,seen_gen)"
           " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,0,?11)",
           &err);
  Stmt insft(db.handle(),
             "INSERT INTO file_text(rowid,name,path_tokens,content,tags)"
             " VALUES(?1,?2,?3,'','')",
             &err);
  Stmt upd(db.handle(),
           "UPDATE files SET size=?1,created_at=?2,modified_at=?3,"
           " indexed_at=?4,fingerprint=?5,missing=0,seen_gen=?6 WHERE id=?7",
           &err);
  Stmt touch(db.handle(),
             "UPDATE files SET missing=0,seen_gen=?1 WHERE id=?2", &err);
  Stmt ins_root(db.handle(), "INSERT OR IGNORE INTO roots(path) VALUES(?1)",
                &err);
  Stmt sel_root(db.handle(), "SELECT id FROM roots WHERE path=?1", &err);
  if (!sel || !ins || !insft || !upd || !touch || !ins_root || !sel_root) {
    std::fprintf(stderr, "crawler: prepare failed: %s\n", err.c_str());
    return;
  }

  // One transaction, committed in batches: results become searchable during
  // the initial crawl and the WAL stays bounded, without per-file fsync cost.
  db.Exec("BEGIN", &err);
  long long in_batch = 0;
  auto maybe_commit = [&]() {
    if (++in_batch >= 1000) {
      db.Exec("COMMIT", &err);
      db.Exec("BEGIN", &err);
      in_batch = 0;
    }
  };

  for (const auto& root : roots) {
    if (stop.load()) break;

    // Ensure the root has a row and fetch its id for new files.
    long long root_id = 0;
    sqlite3_reset(ins_root.get());
    sqlite3_bind_text(ins_root.get(), 1, root.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(ins_root.get());
    sqlite3_reset(sel_root.get());
    sqlite3_bind_text(sel_root.get(), 1, root.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(sel_root.get()) == SQLITE_ROW) {
      root_id = sqlite3_column_int64(sel_root.get(), 0);
    }

    std::error_code ec;
    auto it = fs::recursive_directory_iterator(
        root, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
      std::fprintf(stderr, "crawler: skipping root %s: %s\n", root.c_str(),
                   ec.message().c_str());
      continue;
    }
    const fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
      if (stop.load()) break;
      if (ec) {  // unreadable entry: skip, keep going
        ec.clear();
        continue;
      }
      const fs::directory_entry& e = *it;
      std::error_code sec;

      if (e.is_directory(sec)) {
        if (IsExcludedDir(e.path().filename().string())) {
          it.disable_recursion_pending();
        }
        continue;
      }
      if (!e.is_regular_file(sec)) continue;
      status.scanned.fetch_add(1, std::memory_order_relaxed);

      const std::string ext = LowerExt(e.path());
      const std::string kind = KindForExtension(ext);
      if (kind.empty()) continue;  // not an indexed type

      const std::string path = e.path().string();
      struct stat st{};
      if (::stat(path.c_str(), &st) != 0) continue;

      const std::string name = e.path().filename().string();
      const long long size = static_cast<long long>(st.st_size);
      const long long mtime = static_cast<long long>(st.st_mtimespec.tv_sec);
      const long long ctime = static_cast<long long>(st.st_birthtimespec.tv_sec);
      const std::string fp = std::to_string(mtime) + ":" + std::to_string(size);

      // Does a row already exist for this exact path?
      sqlite3_reset(sel.get());
      sqlite3_bind_text(sel.get(), 1, path.c_str(), -1, SQLITE_TRANSIENT);
      long long existing_id = 0;
      std::string existing_fp;
      if (sqlite3_step(sel.get()) == SQLITE_ROW) {
        existing_id = sqlite3_column_int64(sel.get(), 0);
        const unsigned char* t = sqlite3_column_text(sel.get(), 1);
        if (t != nullptr) existing_fp = reinterpret_cast<const char*>(t);
      }
      sqlite3_reset(sel.get());  // release the read cursor before any commit

      if (existing_id == 0) {
        // New file: insert the row and its FTS entry.
        sqlite3_stmt* s = ins.get();
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
        if (sqlite3_step(s) != SQLITE_DONE) continue;
        const long long id = sqlite3_last_insert_rowid(db.handle());

        const std::string toks = PathTokens(path);
        sqlite3_stmt* f = insft.get();
        sqlite3_reset(f);
        sqlite3_bind_int64(f, 1, id);
        sqlite3_bind_text(f, 2, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(f, 3, toks.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(f) != SQLITE_DONE) continue;
        status.indexed.fetch_add(1, std::memory_order_relaxed);
      } else if (fp != existing_fp) {
        // Changed file: refresh metadata. name/path_tokens derive from the
        // immutable path, so the FTS row needs no update; content/tags fill
        // in at extraction (later brick).
        sqlite3_stmt* s = upd.get();
        sqlite3_reset(s);
        sqlite3_bind_int64(s, 1, size);
        sqlite3_bind_int64(s, 2, ctime);
        sqlite3_bind_int64(s, 3, mtime);
        sqlite3_bind_int64(s, 4, now);
        sqlite3_bind_text(s, 5, fp.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 6, gen);
        sqlite3_bind_int64(s, 7, existing_id);
        if (sqlite3_step(s) != SQLITE_DONE) continue;
        status.indexed.fetch_add(1, std::memory_order_relaxed);
      } else {
        // Unchanged: just mark it seen this generation and clear any
        // stale tombstone -- no content rewrite.
        sqlite3_stmt* s = touch.get();
        sqlite3_reset(s);
        sqlite3_bind_int64(s, 1, gen);
        sqlite3_bind_int64(s, 2, existing_id);
        sqlite3_step(s);
      }
      maybe_commit();
    }
  }

  // A complete pass: tombstone everything not seen this generation (deleted
  // or moved away on disk). Rows stay as tombstones until reconciliation
  // purges them (later brick). A partial pass (stop requested) skips this so
  // we never mass-tombstone live files we simply did not reach.
  if (!stop.load()) {
    Stmt tomb(db.handle(),
              "UPDATE files SET missing=1 WHERE seen_gen < ?1 AND missing=0",
              &err);
    if (tomb) {
      sqlite3_bind_int64(tomb.get(), 1, gen);
      sqlite3_step(tomb.get());
    }
    WriteCrawlGen(db, gen);  // persist only after the pass completes
  }
  db.Exec("COMMIT", &err);
}

}  // namespace macassist
