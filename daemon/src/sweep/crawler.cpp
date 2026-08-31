#include "sweep/crawler.hpp"

#include <sqlite3.h>
#include <sys/stat.h>

#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_set>

#include "index/database.hpp"
#include "index/file_kind.hpp"

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

}  // namespace

CrawlStats CrawlRebuild(Database& db, const std::vector<std::string>& roots) {
  CrawlStats stats;
  std::string err;

  // One big transaction: fast, and the index is never seen half-cleared.
  db.Exec("BEGIN", &err);
  db.Exec("DELETE FROM file_text", &err);
  db.Exec("DELETE FROM files", &err);

  Stmt ins(db.handle(),
           "INSERT OR IGNORE INTO files"
           "(path,name,ext,kind,size,created_at,modified_at,indexed_at,"
           " fingerprint) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)",
           &err);
  Stmt insft(db.handle(),
             "INSERT INTO file_text(rowid,name,path_tokens,content,tags)"
             " VALUES(?1,?2,?3,'','')",
             &err);
  if (!ins || !insft) {
    std::fprintf(stderr, "crawler: prepare failed: %s\n", err.c_str());
    db.Exec("ROLLBACK", &err);
    return stats;
  }

  const long long now = static_cast<long long>(::time(nullptr));

  for (const auto& root : roots) {
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
      ++stats.scanned;

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
      const std::string fp =
          std::to_string(mtime) + ":" + std::to_string(size);

      sqlite3_stmt* s = ins.get();
      sqlite3_reset(s);
      sqlite3_bind_text(s, 1, path.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(s, 2, name.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(s, 3, ext.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(s, 4, kind.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(s, 5, size);
      sqlite3_bind_int64(s, 6, ctime);
      sqlite3_bind_int64(s, 7, mtime);
      sqlite3_bind_int64(s, 8, now);
      sqlite3_bind_text(s, 9, fp.c_str(), -1, SQLITE_TRANSIENT);
      if (sqlite3_step(s) != SQLITE_DONE) continue;
      if (sqlite3_changes(db.handle()) == 0) continue;  // duplicate path
      const long long id = sqlite3_last_insert_rowid(db.handle());

      const std::string toks = PathTokens(path);
      sqlite3_stmt* f = insft.get();
      sqlite3_reset(f);
      sqlite3_bind_int64(f, 1, id);
      sqlite3_bind_text(f, 2, name.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(f, 3, toks.c_str(), -1, SQLITE_TRANSIENT);
      if (sqlite3_step(f) != SQLITE_DONE) continue;
      ++stats.indexed;
    }
  }

  db.Exec("COMMIT", &err);
  return stats;
}

}  // namespace macassist
