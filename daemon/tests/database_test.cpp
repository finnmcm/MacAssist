#include <catch2/catch.hpp>

#include <sqlite3.h>

#include <string>

#include "index/database.hpp"
#include "test_util.hpp"

using namespace macassist;
using macassist::test::TempDir;

namespace {

// Runs a scalar integer query on a raw handle (-1 on error / no row).
long long Scalar(sqlite3* db, const std::string& sql) {
  sqlite3_stmt* s = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) != SQLITE_OK) {
    return -1;
  }
  long long v = (sqlite3_step(s) == SQLITE_ROW) ? sqlite3_column_int64(s, 0) : -1;
  sqlite3_finalize(s);
  return v;
}

bool TableExists(sqlite3* db, const std::string& name) {
  return Scalar(db, "SELECT count(*) FROM sqlite_master WHERE name='" + name +
                        "'") > 0;
}

}  // namespace

TEST_CASE("Open creates the v7 schema", "[database]") {
  TempDir tmp;
  std::string err;
  auto db = Database::Open(tmp.file("index.db"), Access::ReadWrite, &err);
  REQUIRE(db != nullptr);
  CHECK(err.empty());

  CHECK(db->GetMeta("schema_version") == "7");

  sqlite3* h = db->handle();
  for (const char* t :
       {"files", "file_text", "file_meta", "roots", "exclusions", "meta"}) {
    INFO("table " << t);
    CHECK(TableExists(h, t));
  }
  CHECK(TableExists(h, "idx_files_needs_extract"));  // the partial index

  // Fresh DB: no files, nothing pending.
  CHECK(db->CountFiles() == 0);
  CHECK(db->CountPending() == 0);
}

TEST_CASE("meta get/set round-trips", "[database]") {
  TempDir tmp;
  std::string err;
  auto db = Database::Open(tmp.file("index.db"), Access::ReadWrite, &err);
  REQUIRE(db != nullptr);

  CHECK(db->GetMeta("absent") == "");
  REQUIRE(db->SetMeta("crawl_gen", "42", &err));
  CHECK(db->GetMeta("crawl_gen") == "42");
  REQUIRE(db->SetMeta("crawl_gen", "43", &err));  // upsert
  CHECK(db->GetMeta("crawl_gen") == "43");
}

TEST_CASE("CountFiles and CountPending respect missing/needs_extract",
          "[database]") {
  TempDir tmp;
  std::string err;
  auto db = Database::Open(tmp.file("index.db"), Access::ReadWrite, &err);
  REQUIRE(db != nullptr);

  // present + pending
  REQUIRE(db->Exec("INSERT INTO files(path,name,needs_extract,missing) "
                   "VALUES('/a','a',1,0)",
                   &err));
  // present + extracted
  REQUIRE(db->Exec("INSERT INTO files(path,name,needs_extract,missing) "
                   "VALUES('/b','b',0,0)",
                   &err));
  // tombstoned (not counted by either)
  REQUIRE(db->Exec("INSERT INTO files(path,name,needs_extract,missing) "
                   "VALUES('/c','c',1,1)",
                   &err));

  CHECK(db->CountFiles() == 2);    // a, b
  CHECK(db->CountPending() == 1);  // a only
}

TEST_CASE("Version mismatch rebuilds the schema destructively", "[database]") {
  TempDir tmp;
  const std::string path = tmp.file("index.db");
  std::string err;

  {
    auto db = Database::Open(path, Access::ReadWrite, &err);
    REQUIRE(db != nullptr);
    REQUIRE(db->Exec("INSERT INTO files(path,name) VALUES('/keep','keep')",
                     &err));
    REQUIRE(db->CountFiles() == 1);
    // Fake an older schema version so the next open triggers a rebuild.
    REQUIRE(db->SetMeta("schema_version", "3", &err));
  }

  auto db2 = Database::Open(path, Access::ReadWrite, &err);
  REQUIRE(db2 != nullptr);
  CHECK(db2->GetMeta("schema_version") == "7");
  CHECK(db2->CountFiles() == 0);  // rebuilt: prior row cleared
}

TEST_CASE("Reopen at the current version preserves data", "[database]") {
  TempDir tmp;
  const std::string path = tmp.file("index.db");
  std::string err;
  {
    auto db = Database::Open(path, Access::ReadWrite, &err);
    REQUIRE(db != nullptr);
    REQUIRE(db->Exec("INSERT INTO files(path,name) VALUES('/keep','keep')",
                     &err));
  }
  auto db2 = Database::Open(path, Access::ReadWrite, &err);
  REQUIRE(db2 != nullptr);
  CHECK(db2->CountFiles() == 1);  // no rebuild, row survives
}

TEST_CASE("ReadOnly connection forbids writes", "[database]") {
  TempDir tmp;
  const std::string path = tmp.file("index.db");
  std::string err;
  {
    auto rw = Database::Open(path, Access::ReadWrite, &err);
    REQUIRE(rw != nullptr);  // create + migrate first
  }
  auto ro = Database::Open(path, Access::ReadOnly, &err);
  REQUIRE(ro != nullptr);
  // query_only=ON: a write must fail.
  CHECK_FALSE(ro->Exec("INSERT INTO files(path,name) VALUES('/x','x')", nullptr));
}

TEST_CASE("ReadOnly open of a missing database fails", "[database]") {
  TempDir tmp;
  std::string err;
  auto ro = Database::Open(tmp.file("nope.db"), Access::ReadOnly, &err);
  CHECK(ro == nullptr);
}
