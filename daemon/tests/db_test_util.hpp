#pragma once
// Scalar-query helpers for tests that need to peek at raw index state.
#include <sqlite3.h>

#include <string>

#include "index/database.hpp"

namespace macassist::test {

// Integer scalar query on a connection (-1 on error / no row).
inline long long Scalar(Database& db, const std::string& sql) {
  sqlite3_stmt* s = nullptr;
  if (sqlite3_prepare_v2(db.handle(), sql.c_str(), -1, &s, nullptr) !=
      SQLITE_OK) {
    return -1;
  }
  long long v =
      (sqlite3_step(s) == SQLITE_ROW) ? sqlite3_column_int64(s, 0) : -1;
  sqlite3_finalize(s);
  return v;
}

// Text scalar query ("" on error / no row / NULL).
inline std::string TextScalar(Database& db, const std::string& sql) {
  sqlite3_stmt* s = nullptr;
  if (sqlite3_prepare_v2(db.handle(), sql.c_str(), -1, &s, nullptr) !=
      SQLITE_OK) {
    return "";
  }
  std::string v;
  if (sqlite3_step(s) == SQLITE_ROW) {
    const unsigned char* t = sqlite3_column_text(s, 0);
    if (t != nullptr) v = reinterpret_cast<const char*>(t);
  }
  sqlite3_finalize(s);
  return v;
}

}  // namespace macassist::test
