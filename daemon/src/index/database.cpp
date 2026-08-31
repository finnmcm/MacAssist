#include "index/database.hpp"

namespace macassist {

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
                                         std::string* err) {
  sqlite3* db = nullptr;
  const int flags =
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
  if (sqlite3_open_v2(path.c_str(), &db, flags, nullptr) != SQLITE_OK) {
    if (err != nullptr) *err = (db != nullptr) ? sqlite3_errmsg(db) : "oom";
    if (db != nullptr) sqlite3_close(db);
    return nullptr;
  }
  std::unique_ptr<Database> self(new Database(db));
  if (!self->Migrate(err)) return nullptr;
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
  Stmt s(db_, "SELECT count(*) FROM files", &err);
  if (!s) return 0;
  return (sqlite3_step(s.get()) == SQLITE_ROW)
             ? sqlite3_column_int64(s.get(), 0)
             : 0;
}

bool Database::Migrate(std::string* err) {
  // Phase 1/2 schema subset (see PLAN.md section 3). The `content` and
  // `tags` FTS columns exist now but stay empty until content extraction
  // lands, so the query layer need not change when they fill in.
  static const char* kSchema = R"SQL(
    PRAGMA journal_mode=WAL;
    PRAGMA synchronous=NORMAL;
    CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT);
    CREATE TABLE IF NOT EXISTS files(
      id INTEGER PRIMARY KEY,
      path TEXT UNIQUE NOT NULL,
      name TEXT NOT NULL,
      ext TEXT, kind TEXT, size INTEGER,
      created_at INTEGER, modified_at INTEGER, indexed_at INTEGER,
      fingerprint TEXT);
    CREATE VIRTUAL TABLE IF NOT EXISTS file_text USING fts5(
      name, path_tokens, content, tags, tokenize='unicode61');
    INSERT OR IGNORE INTO meta(key,value) VALUES('schema_version','1');
  )SQL";
  return Exec(kSchema, err);
}

}  // namespace macassist
