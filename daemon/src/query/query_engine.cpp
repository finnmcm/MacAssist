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

// SQL predicate for the active filters, ANDed onto the WHERE. Each group uses
// bound '?' placeholders (never interpolated values), so the IN-lists can't
// inject. The tag-color group must guard tags_json before json_each: untagged
// files store '' (not NULL), and json_each('') raises "malformed JSON" and
// aborts the whole query -- json_each(NULL) is safe, json_each('') is not.
std::string FilterSql(const QueryFilters& f) {
  std::string s;
  if (!f.kinds.empty()) {
    s += " AND f.kind IN (";
    for (std::size_t i = 0; i < f.kinds.size(); ++i) s += (i ? ",?" : "?");
    s += ")";
  }
  if (!f.tag_colors.empty()) {
    s += " AND f.tags_json IS NOT NULL AND f.tags_json <> '' "
         "AND json_valid(f.tags_json) "
         "AND EXISTS (SELECT 1 FROM json_each(f.tags_json) "
         "WHERE json_extract(value, '$.color') IN (";
    for (std::size_t i = 0; i < f.tag_colors.size(); ++i)
      s += (i ? ",?" : "?");
    s += "))";
  }
  if (f.downloaded_only) s += " AND f.downloaded_at IS NOT NULL";
  return s;
}

// Binds the FilterSql placeholders in the same order FilterSql emits them
// (kinds, then tag colors), advancing *idx. downloaded_only has no binds.
// std::set iteration is deterministic, so the SQL and the binds stay aligned.
void BindFilters(sqlite3_stmt* s, int* idx, const QueryFilters& f) {
  for (const std::string& k : f.kinds)
    sqlite3_bind_text(s, (*idx)++, k.c_str(), -1, SQLITE_TRANSIENT);
  for (int c : f.tag_colors) sqlite3_bind_int(s, (*idx)++, c);
}

void ReadHits(sqlite3_stmt* s, std::vector<FileHit>* hits) {
  while (sqlite3_step(s) == SQLITE_ROW) {
    FileHit h;
    h.id = sqlite3_column_int64(s, 0);
    h.path = ColText(s, 1);
    h.name = ColText(s, 2);
    h.kind = ColText(s, 3);
    h.modified_at = sqlite3_column_int64(s, 4);
    h.tags_json = ColText(s, 5);
    h.score = sqlite3_column_double(s, 6);
    hits->push_back(std::move(h));
  }
}

}  // namespace

std::vector<FileHit> SearchFiles(Database& db, const std::string& text,
                                 int limit, const QueryFilters& filters) {
  std::vector<FileHit> hits;
  const std::string match = BuildMatch(text);
  const std::string filter_sql = FilterSql(filters);

  if (match.empty()) {
    // No searchable text. With no filters either, there's nothing to return
    // (matches the historical empty-query behavior). With filters, list the
    // matching files directly (no FTS join) ordered by recency.
    if (filters.empty()) return hits;

    std::string sql =
        "SELECT f.id, f.path, f.name, f.kind, f.modified_at, f.tags_json, "
        "  0.0 AS score "
        "FROM files f WHERE f.missing = 0";
    sql += filter_sql;
    sql += filters.downloaded_only ? " ORDER BY f.downloaded_at DESC"
                                   : " ORDER BY f.modified_at DESC";
    sql += " LIMIT ?";

    std::string err;
    Stmt s(db.handle(), sql, &err);
    if (!s) return hits;
    int idx = 1;
    BindFilters(s.get(), &idx, filters);
    sqlite3_bind_int(s.get(), idx, limit);
    ReadHits(s.get(), &hits);
    return hits;
  }

  // The same tokens restricted to the name column, so we can tell a direct
  // filename hit from a content/path-only hit. FTS5 column filter: the group
  // after `{name} :` is matched against the `name` column only.
  const std::string name_match = "{name} : (" + match + ")";

  // Two-tier ranking: a direct name match always ranks above a hit that only
  // matched content/path/tags -- so "open my_photo" surfaces the file named
  // my_photo before any document that merely mentions it. `name_hit` is
  // non-NULL for rows whose name matched (via a LEFT JOIN to a name-only
  // match); `name_hit IS NULL` sorts those first, and bm25 (name>path>tags>
  // content weights) orders within a tier. Tiering is in the ORDER BY, so it
  // applies before LIMIT. (FTS5 MATCH rejects a table alias, hence the derived
  // table rather than a correlated subquery.)
  //
  // Placeholders bind in textual order: name_match (derived table), match
  // (WHERE), then any filter binds, then the limit.
  std::string sql =
      "SELECT f.id, f.path, f.name, f.kind, f.modified_at, f.tags_json, "
      "  bm25(file_text, 10.0, 4.0, 2.0, 3.0) AS score, "
      "  nmz.rowid AS name_hit "
      "FROM file_text ft JOIN files f ON f.id = ft.rowid "
      "LEFT JOIN (SELECT rowid FROM file_text WHERE file_text MATCH ?) nmz "
      "  ON nmz.rowid = ft.rowid "
      "WHERE file_text MATCH ? AND f.missing = 0";
  sql += filter_sql;
  sql += " ORDER BY (name_hit IS NULL), score ASC LIMIT ?";

  std::string err;
  Stmt s(db.handle(), sql, &err);
  if (!s) return hits;
  int idx = 1;
  sqlite3_bind_text(s.get(), idx++, name_match.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(s.get(), idx++, match.c_str(), -1, SQLITE_TRANSIENT);
  BindFilters(s.get(), &idx, filters);
  sqlite3_bind_int(s.get(), idx, limit);
  ReadHits(s.get(), &hits);
  return hits;
}

}  // namespace macassist
