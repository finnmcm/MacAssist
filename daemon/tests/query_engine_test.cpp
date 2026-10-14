#include <catch2/catch.hpp>

#include <string>
#include <vector>

#include "index/database.hpp"
#include "query/query_engine.hpp"
#include "test_util.hpp"

using namespace macassist;
using macassist::test::TempDir;

namespace {

// Insert a synthetic indexed file + its FTS row with exact column control, so
// ranking/matching can be tested without touching the filesystem.
void AddFile(Database& db, long long id, const std::string& name,
             const std::string& path_tokens, const std::string& content,
             const std::string& tags = "", int missing = 0) {
  std::string err;
  const std::string sid = std::to_string(id);
  REQUIRE(db.Exec("INSERT INTO files(id,path,name,kind,missing,needs_extract) "
                  "VALUES(" +
                      sid + ",'/p/" + sid + "/" + name + "','" + name +
                      "','document'," + std::to_string(missing) + ",0)",
                  &err));
  sqlite3_stmt* s = nullptr;
  REQUIRE(sqlite3_prepare_v2(
              db.handle(),
              "INSERT INTO file_text(rowid,name,path_tokens,content,tags) "
              "VALUES(?1,?2,?3,?4,?5)",
              -1, &s, nullptr) == SQLITE_OK);
  sqlite3_bind_int64(s, 1, id);
  sqlite3_bind_text(s, 2, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(s, 3, path_tokens.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(s, 4, content.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(s, 5, tags.c_str(), -1, SQLITE_TRANSIENT);
  REQUIRE(sqlite3_step(s) == SQLITE_DONE);
  sqlite3_finalize(s);
}

std::vector<std::string> Names(const std::vector<FileHit>& hits) {
  std::vector<std::string> out;
  for (const auto& h : hits) out.push_back(h.name);
  return out;
}

struct QueryFixture {
  TempDir tmp;
  std::unique_ptr<Database> db;
  std::string err;
  QueryFixture() {
    db = Database::Open(tmp.file("index.db"), Access::ReadWrite, &err);
  }
};

}  // namespace

TEST_CASE("A direct name match ranks above a content-only match",
          "[query][ranking]") {
  QueryFixture f;
  REQUIRE(f.db != nullptr);
  // id 1: name contains the term; body is unrelated.
  AddFile(*f.db, 1, "report.txt", "p 1 report txt", "budget figures attached");
  // id 2: name unrelated; body mentions the term several times (strong bm25).
  AddFile(*f.db, 2, "misc.txt", "p 2 misc txt",
          "quarterly report summary report numbers report");

  auto hits = SearchFiles(*f.db, "report", 10);
  REQUIRE(hits.size() == 2);
  CHECK(hits[0].name == "report.txt");  // name tier wins regardless of bm25
  CHECK(hits[1].name == "misc.txt");
}

TEST_CASE("Content, path, and tag matches are all found", "[query]") {
  QueryFixture f;
  REQUIRE(f.db != nullptr);
  AddFile(*f.db, 1, "a.txt", "p 1 a txt", "the plumbus is here");       // content
  AddFile(*f.db, 2, "b.txt", "p 2 taxes b txt", "unrelated");          // path
  AddFile(*f.db, 3, "c.m4a", "p 3 c m4a", "", "Radiohead OK Computer"); // tags

  CHECK(Names(SearchFiles(*f.db, "plumbus", 10)) ==
        std::vector<std::string>{"a.txt"});
  CHECK(Names(SearchFiles(*f.db, "taxes", 10)) ==
        std::vector<std::string>{"b.txt"});
  CHECK(Names(SearchFiles(*f.db, "radiohead", 10)) ==
        std::vector<std::string>{"c.m4a"});
}

TEST_CASE("Tombstoned files are excluded from results", "[query]") {
  QueryFixture f;
  REQUIRE(f.db != nullptr);
  AddFile(*f.db, 1, "live.txt", "p 1 live txt", "zebra", "", /*missing=*/0);
  AddFile(*f.db, 2, "dead.txt", "p 2 dead txt", "zebra", "", /*missing=*/1);

  CHECK(Names(SearchFiles(*f.db, "zebra", 10)) ==
        std::vector<std::string>{"live.txt"});
}

TEST_CASE("limit caps the number of hits", "[query]") {
  QueryFixture f;
  REQUIRE(f.db != nullptr);
  for (int i = 1; i <= 5; ++i) {
    AddFile(*f.db, i, "note" + std::to_string(i) + ".txt",
            "p " + std::to_string(i) + " common note", "body");
  }
  CHECK(SearchFiles(*f.db, "note", 3).size() == 3);
  CHECK(SearchFiles(*f.db, "note", 10).size() == 5);
}

TEST_CASE("Prefix matching: a partial term matches longer tokens", "[query]") {
  QueryFixture f;
  REQUIRE(f.db != nullptr);
  AddFile(*f.db, 1, "resume.pdf", "p 1 resume pdf", "experience");
  CHECK(Names(SearchFiles(*f.db, "resum", 10)) ==
        std::vector<std::string>{"resume.pdf"});
}

TEST_CASE("Garbage / empty queries return nothing, never crash", "[query]") {
  QueryFixture f;
  REQUIRE(f.db != nullptr);
  AddFile(*f.db, 1, "a.txt", "p 1 a txt", "content");

  CHECK(SearchFiles(*f.db, "", 10).empty());
  CHECK(SearchFiles(*f.db, "   ", 10).empty());
  CHECK(SearchFiles(*f.db, "!@#$%^&*()", 10).empty());
  // FTS operator characters must be sanitized, not injected.
  CHECK_NOTHROW(SearchFiles(*f.db, "a AND \"b* OR (", 10));
}
