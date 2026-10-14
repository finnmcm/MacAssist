#include <catch2/catch.hpp>

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

#include "db_test_util.hpp"
#include "index/database.hpp"
#include "sweep/crawler.hpp"
#include "sweep/indexer.hpp"
#include "sweep/politeness.hpp"
#include "sweep/sweep_status.hpp"
#include "test_util.hpp"

namespace fs = std::filesystem;
using namespace macassist;
using macassist::test::Scalar;
using macassist::test::TempDir;
using macassist::test::WriteFile;

namespace {
struct CrawlFixture {
  TempDir tmp;
  std::unique_ptr<Database> db;
  std::string err;
  SweepStatus status;
  std::atomic<bool> stop{false};
  Politeness politeness;

  CrawlFixture() {
    db = Database::Open(tmp.file("index.db"), Access::ReadWrite, &err);
  }
  std::string root() const { return (tmp.path() / "root").string(); }
  void crawl(Indexer& idx) {
    CrawlIncremental(idx, *db, {root()}, status, stop, politeness);
  }
};
}  // namespace

TEST_CASE("Crawl indexes indexed-type files, honoring exclusions", "[crawler]") {
  CrawlFixture f;
  REQUIRE(f.db != nullptr);
  WriteFile((fs::path(f.root()) / "one.txt").string(), "a");
  WriteFile((fs::path(f.root()) / "sub" / "two.md").string(), "b");
  WriteFile((fs::path(f.root()) / "report.pdf").string(), "%PDF-1.4");
  WriteFile((fs::path(f.root()) / "archive.xyz").string(), "c");  // not indexed
  WriteFile((fs::path(f.root()) / ".git" / "config").string(), "x");  // excluded

  Indexer idx(*f.db);
  REQUIRE(idx.valid());
  f.crawl(idx);

  // one.txt, two.md, report.pdf -> 3; .xyz skipped, .git excluded.
  CHECK(f.db->CountFiles() == 3);
  CHECK(Scalar(*f.db, "SELECT count(*) FROM files WHERE path LIKE '%archive.xyz'") ==
        0);
  CHECK(Scalar(*f.db, "SELECT count(*) FROM files WHERE path LIKE '%.git%'") == 0);
  // Freshly crawled files all await extraction.
  CHECK(f.db->CountPending() == 3);
}

TEST_CASE("Re-crawl tombstones deletions and picks up new files", "[crawler]") {
  CrawlFixture f;
  REQUIRE(f.db != nullptr);
  const std::string a = (fs::path(f.root()) / "a.txt").string();
  const std::string b = (fs::path(f.root()) / "b.txt").string();
  WriteFile(a, "a");
  WriteFile(b, "b");

  Indexer idx(*f.db);
  REQUIRE(idx.valid());
  f.crawl(idx);
  REQUIRE(f.db->CountFiles() == 2);

  SECTION("deletion is tombstoned on the next pass") {
    fs::remove(a);
    f.crawl(idx);
    CHECK(f.db->CountFiles() == 1);
    CHECK(Scalar(*f.db, "SELECT missing FROM files WHERE path='" + a + "'") == 1);
  }

  SECTION("a new file is added on the next pass") {
    WriteFile((fs::path(f.root()) / "c.txt").string(), "c");
    f.crawl(idx);
    CHECK(f.db->CountFiles() == 3);
  }

  SECTION("an unchanged re-crawl keeps the same count") {
    f.crawl(idx);
    CHECK(f.db->CountFiles() == 2);
  }
}

TEST_CASE("A stopped crawl does not mass-tombstone unreached files",
          "[crawler]") {
  CrawlFixture f;
  REQUIRE(f.db != nullptr);
  WriteFile((fs::path(f.root()) / "a.txt").string(), "a");
  WriteFile((fs::path(f.root()) / "b.txt").string(), "b");

  Indexer idx(*f.db);
  REQUIRE(idx.valid());
  f.crawl(idx);
  REQUIRE(f.db->CountFiles() == 2);

  // A pass that is stopped before completion must skip the stale-tombstone
  // sweep, so already-indexed files are not wrongly marked missing.
  f.stop.store(true);
  f.crawl(idx);
  CHECK(f.db->CountFiles() == 2);
}
