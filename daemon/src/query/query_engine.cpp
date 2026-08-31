#include "query/query_engine.hpp"

#include <sqlite3.h>

#include <cctype>

#include "index/database.hpp"

namespace macassist {
namespace {

// Turns free text into a safe FTS5 MATCH expression: alphanumeric tokens
// each given a trailing '*' for prefix matching, implicitly ANDed. All
// FTS operator characters are dropped, so arbitrary user input cannot
// form a malformed (or injected) MATCH expression. Returns "" if nothing
// usable remains.
std::string BuildMatch(const std::string& text) {
  std::string out;
  std::string tok;
  auto flush = [&]() {
    if (tok.empty()) return;
    if (!out.empty()) out.push_back(' ');
    out += tok;
    out.push_back('*');
    tok.clear();
  };
  for (char c : text) {
    if (std::isalnum((unsigned char)c)) {
      tok.push_back(c);
    } else {
      flush();
    }
  }
  flush();
  return out;
}

const char* ColText(sqlite3_stmt* s, int col) {
  const unsigned char* t = sqlite3_column_text(s, col);
  return (t != nullptr) ? reinterpret_cast<const char*>(t) : "";
}

}  // namespace

std::vector<FileHit> SearchFiles(Database& db, const std::string& text,
                                 int limit) {
  std::vector<FileHit> hits;
  const std::string match = BuildMatch(text);
  if (match.empty()) return hits;

  // bm25 weights favor a name hit over a deep-path hit; content/tags are
  // in the middle so they contribute once extraction fills them in.
  std::string err;
  Stmt s(db.handle(),
         "SELECT f.id, f.path, f.name, f.kind, f.modified_at, "
         "  bm25(file_text, 10.0, 4.0, 2.0, 3.0) AS score "
         "FROM file_text ft JOIN files f ON f.id = ft.rowid "
         "WHERE file_text MATCH ?1 "
         "ORDER BY score ASC LIMIT ?2",
         &err);
  if (!s) return hits;
  sqlite3_bind_text(s.get(), 1, match.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(s.get(), 2, limit);

  while (sqlite3_step(s.get()) == SQLITE_ROW) {
    FileHit h;
    h.id = sqlite3_column_int64(s.get(), 0);
    h.path = ColText(s.get(), 1);
    h.name = ColText(s.get(), 2);
    h.kind = ColText(s.get(), 3);
    h.modified_at = sqlite3_column_int64(s.get(), 4);
    h.score = sqlite3_column_double(s.get(), 5);
    hits.push_back(std::move(h));
  }
  return hits;
}

}  // namespace macassist
