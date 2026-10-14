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

// Richer insert for the filter tests: explicit kind, modified_at, files-level
// tags_json (the filter column -- distinct from file_text.tags above), and an
// optional downloaded_at (0 == leave NULL). A minimal FTS row (name only) is
// added so text+filter queries can still match.
void AddRow(Database& db, long long id, const std::string& name,
            const std::string& kind, long long modified_at,
            const std::string& tags_json = "", long long downloaded_at = 0) {
  std::string err;
  const std::string sid = std::to_string(id);
  const std::string dl = downloaded_at ? std::to_string(downloaded_at) : "NULL";
  REQUIRE(db.Exec(
      "INSERT INTO files(id,path,name,kind,modified_at,tags_json,downloaded_at,"
      "missing,needs_extract) VALUES(" +
          sid + ",'/p/" + sid + "/" + name + "','" + name + "','" + kind +
          "'," + std::to_string(modified_at) + ",'" + tags_json + "'," + dl +
          ",0,0)",
      &err));
  sqlite3_stmt* s = nullptr;
  REQUIRE(sqlite3_prepare_v2(
              db.handle(),
              "INSERT INTO file_text(rowid,name,path_tokens,content,tags) "
              "VALUES(?1,?2,?2,'','')",
              -1, &s, nullptr) == SQLITE_OK);
  sqlite3_bind_int64(s, 1, id);
  sqlite3_bind_text(s, 2, name.c_str(), -1, SQLITE_TRANSIENT);
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

// ---------------------------------------------------------------------------
// Filter toolbar (Brick B)
// ---------------------------------------------------------------------------

TEST_CASE("Empty text + kind filter returns that kind, newest first",
          "[query][filters]") {
  QueryFixture f;
  REQUIRE(f.db != nullptr);
  AddRow(*f.db, 1, "song1.m4a", "audio", 100);
  AddRow(*f.db, 2, "song2.m4a", "audio", 200);
  AddRow(*f.db, 3, "pic.jpg", "image", 300);

  // Guards the Music->audio mapping: the canonical kind "audio" (not the UI
  // label) returns the audio rows, ordered modified_at DESC.
  QueryFilters qf;
  qf.kinds = {"audio"};
  CHECK(Names(SearchFiles(*f.db, "", 10, qf)) ==
        std::vector<std::string>{"song2.m4a", "song1.m4a"});
}

TEST_CASE("Tag filter skips untagged files without erroring",
          "[query][filters]") {
  QueryFixture f;
  REQUIRE(f.db != nullptr);
  AddRow(*f.db, 1, "red.txt", "document", 100,
         R"([{"name":"Red","color":6}])");
  AddRow(*f.db, 2, "none.txt", "document", 200, "");  // empty tags_json ('')
  AddRow(*f.db, 3, "blue.txt", "document", 300,
         R"([{"name":"Blue","color":4}])");

  QueryFilters qf;
  qf.tag_colors = {6};
  std::vector<FileHit> hits;
  // json_each('') would abort the query; the guard must prevent that.
  REQUIRE_NOTHROW(hits = SearchFiles(*f.db, "", 10, qf));
  CHECK(Names(hits) == std::vector<std::string>{"red.txt"});
}

TEST_CASE("Downloaded-only filter returns downloads, downloaded_at DESC",
          "[query][filters]") {
  QueryFixture f;
  REQUIRE(f.db != nullptr);
  AddRow(*f.db, 1, "a.pdf", "document", 500, "", /*downloaded_at=*/1000);
  AddRow(*f.db, 2, "b.pdf", "document", 400, "", /*downloaded_at=*/3000);
  AddRow(*f.db, 3, "c.pdf", "document", 300);  // not a download (NULL)

  QueryFilters qf;
  qf.downloaded_only = true;
  // Ordered by downloaded_at DESC, not modified_at.
  CHECK(Names(SearchFiles(*f.db, "", 10, qf)) ==
        std::vector<std::string>{"b.pdf", "a.pdf"});
}

TEST_CASE("Mixed filters: same-type OR, cross-type AND", "[query][filters]") {
  QueryFixture f;
  REQUIRE(f.db != nullptr);
  AddRow(*f.db, 1, "p.jpg", "image", 100, R"([{"name":"Red","color":6}])");
  AddRow(*f.db, 2, "v.mp4", "video", 200, R"([{"name":"Red","color":6}])");
  AddRow(*f.db, 3, "d.pdf", "document", 300,
         R"([{"name":"Red","color":6}])");  // wrong kind
  AddRow(*f.db, 4, "p2.jpg", "image", 400,
         R"([{"name":"Blue","color":4}])");  // wrong tag

  // (image OR video) AND (tag color 6): matches ids 1 and 2 only.
  QueryFilters qf;
  qf.kinds = {"image", "video"};
  qf.tag_colors = {6};
  CHECK(Names(SearchFiles(*f.db, "", 10, qf)) ==
        std::vector<std::string>{"v.mp4", "p.jpg"});
}

TEST_CASE("Text and a filter intersect", "[query][filters]") {
  QueryFixture f;
  REQUIRE(f.db != nullptr);
  AddRow(*f.db, 1, "report.m4a", "audio", 100);
  AddRow(*f.db, 2, "report.pdf", "document", 200);

  QueryFilters qf;
  qf.kinds = {"audio"};
  CHECK(Names(SearchFiles(*f.db, "report", 10, qf)) ==
        std::vector<std::string>{"report.m4a"});
}

TEST_CASE("Empty text with no filters still returns nothing",
          "[query][filters]") {
  QueryFixture f;
  REQUIRE(f.db != nullptr);
  AddRow(*f.db, 1, "a.pdf", "document", 100);
  CHECK(SearchFiles(*f.db, "", 10, QueryFilters{}).empty());
}
