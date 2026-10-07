#include "index/database.hpp"

#include <cstdio>
#include <cstdlib>

namespace macassist {
namespace {

constexpr int kSchemaVersion = 3;

// Reads meta.schema_version; returns 0 if the meta table or row is absent
// (e.g. a brand-new database), which forces a rebuild to the current schema.
int ReadSchemaVersion(sqlite3* db) {
  sqlite3_stmt* s = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT value FROM meta WHERE key='schema_version'",
                         -1, &s, nullptr) != SQLITE_OK) {
    return 0;
  }
  int v = 0;
  if (sqlite3_step(s) == SQLITE_ROW) {
    const unsigned char* t = sqlite3_column_text(s, 0);
    if (t != nullptr) v = std::atoi(reinterpret_cast<const char*>(t));
  }
  sqlite3_finalize(s);
  return v;
}

}  // namespace

Stmt::Stmt(sqlite3* db, const std::string& sql, std::string* err) {
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK) {
    if (err != nullptr) *err = sqlite3_errmsg(db);
    stmt_ = nullptr;
  }
}

Stmt::~Stmt() {
  if (stmt_ != nullptr) sqlite3_finalize(stmt_);
}

Stmt::Stmt(Stmt&& other) noexcept : stmt_(other.stmt_) { other.stmt_ = nullptr; }

Stmt& Stmt::operator=(Stmt&& other) noexcept {
  if (this != &other) {
    if (stmt_ != nullptr) sqlite3_finalize(stmt_);
    stmt_ = other.stmt_;
    other.stmt_ = nullptr;
  }
  return *this;
}

std::unique_ptr<Database> Database::Open(const std::string& path,
                                         Access access, std::string* err) {
  // NOMUTEX: each connection is owned by a single thread (the sweeper for
  // ReadWrite, the IPC loop for ReadOnly), so we skip SQLite's per-call
  // serialization. WAL -- not a lock -- coordinates reader and writer.
  int flags = SQLITE_OPEN_NOMUTEX;
  flags |= (access == Access::ReadWrite)
               ? (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)
               : SQLITE_OPEN_READONLY;

  sqlite3* db = nullptr;
  if (sqlite3_open_v2(path.c_str(), &db, flags, nullptr) != SQLITE_OK) {
    if (err != nullptr) *err = (db != nullptr) ? sqlite3_errmsg(db) : "oom";
    if (db != nullptr) sqlite3_close(db);
    return nullptr;
  }
  std::unique_ptr<Database> self(new Database(db));

  if (access == Access::ReadWrite) {
    // journal_mode/synchronous are set on the writer; migrate with foreign
    // keys off (simpler drops), then enforce them for the session.
    if (!self->Exec("PRAGMA journal_mode=WAL;PRAGMA synchronous=NORMAL;",
                    err) ||
        !self->Migrate(err) || !self->Exec("PRAGMA foreign_keys=ON;", err)) {
      return nullptr;
    }
  } else {
    // Defensive: a bug in the query path cannot mutate the index.
    if (!self->Exec("PRAGMA query_only=ON;", err)) return nullptr;
  }
  return self;
}

Database::~Database() {
  if (db_ != nullptr) sqlite3_close(db_);
}

bool Database::Exec(const std::string& sql, std::string* err) {
  char* emsg = nullptr;
  if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &emsg) != SQLITE_OK) {
    if (err != nullptr) *err = (emsg != nullptr) ? emsg : "exec failed";
    sqlite3_free(emsg);
    return false;
  }
  return true;
}

long long Database::CountFiles() {
  std::string err;
  Stmt s(db_, "SELECT count(*) FROM files WHERE missing=0", &err);
  if (!s) return 0;
  return (sqlite3_step(s.get()) == SQLITE_ROW)
             ? sqlite3_column_int64(s.get(), 0)
             : 0;
}

bool Database::Migrate(std::string* err) {
  if (ReadSchemaVersion(db_) == kSchemaVersion) return true;

  // Pre-release policy (PLAN.md sections 1/10): no incremental ALTER
  // migrations. Any version mismatch -- including a fresh database that
  // reads as version 0 -- drops the known tables and recreates the current
  // schema; the next crawl repopulates. Destructive by design. The `content`
  // and `tags` FTS columns exist now but stay empty until extraction lands,
  // so the query layer need not change when they fill in.
  std::fprintf(stderr,
               "macassistd: building index schema v%d (any prior data cleared)\n",
               kSchemaVersion);
  static const char* kRebuild = R"SQL(
    BEGIN;
    DROP TABLE IF EXISTS file_text;
    DROP TABLE IF EXISTS files;
    DROP TABLE IF EXISTS roots;
    DROP TABLE IF EXISTS exclusions;
    DROP TABLE IF EXISTS meta;
    CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT);
    CREATE TABLE roots(
      id INTEGER PRIMARY KEY,
      path TEXT UNIQUE NOT NULL,
      enabled INTEGER NOT NULL DEFAULT 1);
    CREATE TABLE exclusions(
      id INTEGER PRIMARY KEY,
      glob TEXT NOT NULL);
    CREATE TABLE files(
      id INTEGER PRIMARY KEY,
      root_id INTEGER REFERENCES roots(id),
      path TEXT UNIQUE NOT NULL,
      name TEXT NOT NULL,
      ext TEXT, kind TEXT, size INTEGER,
      created_at INTEGER, modified_at INTEGER, indexed_at INTEGER,
      fingerprint TEXT,
      missing INTEGER NOT NULL DEFAULT 0,
      seen_gen INTEGER NOT NULL DEFAULT 0);
    CREATE VIRTUAL TABLE file_text USING fts5(
      name, path_tokens, content, tags, tokenize='unicode61');
    COMMIT;
  )SQL";
  if (!Exec(kRebuild, err)) return false;
  // Record version from the constant so the schema and version can't drift.
  return Exec("INSERT INTO meta(key,value) VALUES('schema_version','" +
                  std::to_string(kSchemaVersion) + "')",
              err);
}

}  // namespace macassist
