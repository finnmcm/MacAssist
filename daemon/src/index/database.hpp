#pragma once
#include <sqlite3.h>

#include <memory>
#include <string>

namespace macassist {

// RAII wrapper for a prepared statement. Move-only; finalizes on destroy.
class Stmt {
 public:
  Stmt() = default;
  Stmt(sqlite3* db, const std::string& sql, std::string* err);
  ~Stmt();
  Stmt(Stmt&& other) noexcept;
  Stmt& operator=(Stmt&& other) noexcept;
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;

  sqlite3_stmt* get() const { return stmt_; }
  explicit operator bool() const { return stmt_ != nullptr; }

 private:
  sqlite3_stmt* stmt_ = nullptr;
};

// RAII wrapper around a SQLite connection with the MacAssist schema.
class Database {
 public:
  // Opens (creating if needed) the database at `path`, applies pragmas
  // and migrations. Returns nullptr and sets *err on failure.
  static std::unique_ptr<Database> Open(const std::string& path,
                                        std::string* err);
  ~Database();
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  sqlite3* handle() const { return db_; }

  // Runs one or more semicolon-separated statements with no results.
  bool Exec(const std::string& sql, std::string* err);

  // Row count of the files table (0 on error).
  long long CountFiles();

 private:
  explicit Database(sqlite3* db) : db_(db) {}
  bool Migrate(std::string* err);

  sqlite3* db_ = nullptr;
};

}  // namespace macassist
