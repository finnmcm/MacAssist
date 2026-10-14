#include <catch2/catch.hpp>

#include <chrono>
#include <string>
#include <thread>

#include "db_test_util.hpp"
#include "extract/text_extractors.hpp"
#include "index/database.hpp"
#include "sweep/indexer.hpp"
#include "test_util.hpp"

using namespace macassist;
using macassist::test::Scalar;
using macassist::test::TempDir;
using macassist::test::TextScalar;
using macassist::test::WriteFile;

namespace {
// A Database + Indexer over a fresh temp index, plus a corpus dir for the
// real files UpsertFile stats.
struct Fixture {
  TempDir tmp;
  std::unique_ptr<Database> db;
  std::string err;
  Fixture() {
    db = Database::Open(tmp.file("index.db"), Access::ReadWrite, &err);
  }
  std::string corpus(const std::string& name) const {
    return (tmp.path() / "corpus" / name).string();
  }
};
}  // namespace

TEST_CASE("EnsureRoot is idempotent and returns a stable id", "[indexer]") {
  Fixture f;
  REQUIRE(f.db != nullptr);
  Indexer idx(*f.db);
  REQUIRE(idx.valid());

  long long id = idx.EnsureRoot("/tmp/root");
  CHECK(id > 0);
  CHECK(idx.EnsureRoot("/tmp/root") == id);  // same row, not a duplicate
  CHECK(Scalar(*f.db, "SELECT count(*) FROM roots") == 1);
}

TEST_CASE("UpsertFile inserts, updates on change, skips unchanged",
          "[indexer]") {
  Fixture f;
  REQUIRE(f.db != nullptr);
  Indexer idx(*f.db);
  REQUIRE(idx.valid());
  const long long root = idx.EnsureRoot(f.corpus(""));

  const std::string path = f.corpus("doc.txt");
  WriteFile(path, "hello world");

  SECTION("insert sets needs_extract and an empty FTS row") {
    CHECK(idx.UpsertFile(path, root, 1) == Indexer::Result::kInserted);
    CHECK(Scalar(*f.db, "SELECT count(*) FROM files WHERE missing=0") == 1);
    CHECK(Scalar(*f.db,
                 "SELECT needs_extract FROM files WHERE path='" + path + "'") ==
          1);
    CHECK(TextScalar(*f.db, "SELECT kind FROM files WHERE path='" + path +
                                "'") == "document");
    // FTS row exists with name populated, content still empty.
    CHECK(TextScalar(*f.db, "SELECT name FROM file_text WHERE rowid=(SELECT id "
                            "FROM files WHERE path='" +
                                path + "')") == "doc.txt");
  }

  SECTION("unchanged file is a no-op upsert") {
    REQUIRE(idx.UpsertFile(path, root, 1) == Indexer::Result::kInserted);
    CHECK(idx.UpsertFile(path, root, 2) == Indexer::Result::kUnchanged);
    // seen_gen advanced even though nothing else changed.
    CHECK(Scalar(*f.db, "SELECT seen_gen FROM files WHERE path='" + path +
                            "'") == 2);
  }

  SECTION("a content change re-flags for extraction") {
    REQUIRE(idx.UpsertFile(path, root, 1) == Indexer::Result::kInserted);
    // Simulate "already extracted", then modify the file.
    REQUIRE(f.db->Exec(
        "UPDATE files SET needs_extract=0 WHERE path='" + path + "'", &f.err));
    // Ensure a different mtime/size so the fingerprint changes.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    WriteFile(path, "hello world, now longer");

    CHECK(idx.UpsertFile(path, root, 3) == Indexer::Result::kUpdated);
    CHECK(Scalar(*f.db, "SELECT needs_extract FROM files WHERE path='" + path +
                            "'") == 1);
  }
}

TEST_CASE("UpsertFile skips non-indexed types and reports missing",
          "[indexer]") {
  Fixture f;
  REQUIRE(f.db != nullptr);
  Indexer idx(*f.db);
  const long long root = idx.EnsureRoot(f.corpus(""));

  const std::string other = f.corpus("archive.xyz");
  WriteFile(other, "data");
  CHECK(idx.UpsertFile(other, root, 1) == Indexer::Result::kSkipped);
  CHECK(Scalar(*f.db, "SELECT count(*) FROM files") == 0);

  CHECK(idx.UpsertFile(f.corpus("ghost.txt"), root, 1) ==
        Indexer::Result::kMissing);
}

TEST_CASE("Tombstoning marks rows missing", "[indexer]") {
  Fixture f;
  REQUIRE(f.db != nullptr);
  Indexer idx(*f.db);
  const long long root = idx.EnsureRoot(f.corpus(""));

  const std::string a = f.corpus("a.txt");
  const std::string b = f.corpus("b.txt");
  WriteFile(a, "a");
  WriteFile(b, "b");

  SECTION("TombstoneByPath marks one file") {
    REQUIRE(idx.UpsertFile(a, root, 1) == Indexer::Result::kInserted);
    idx.TombstoneByPath(a);
    CHECK(Scalar(*f.db, "SELECT missing FROM files WHERE path='" + a + "'") ==
          1);
    CHECK(f.db->CountFiles() == 0);
  }

  SECTION("TombstoneStale marks everything from an older generation") {
    REQUIRE(idx.UpsertFile(a, root, 5) == Indexer::Result::kInserted);
    REQUIRE(idx.UpsertFile(b, root, 5) == Indexer::Result::kInserted);
    // Re-see only `a` at gen 6; `b` is now stale.
    REQUIRE(idx.UpsertFile(a, root, 6) == Indexer::Result::kUnchanged);
    idx.TombstoneStale(6);
    CHECK(Scalar(*f.db, "SELECT missing FROM files WHERE path='" + a + "'") ==
          0);
    CHECK(Scalar(*f.db, "SELECT missing FROM files WHERE path='" + b + "'") ==
          1);
  }
}

TEST_CASE("ApplyExtraction writes content/tags/meta and clears the flag",
          "[indexer]") {
  Fixture f;
  REQUIRE(f.db != nullptr);
  Indexer idx(*f.db);
  const long long root = idx.EnsureRoot(f.corpus(""));
  const std::string path = f.corpus("song.m4a");
  WriteFile(path, "ignored bytes");
  REQUIRE(idx.UpsertFile(path, root, 1) == Indexer::Result::kInserted);

  const long long id =
      Scalar(*f.db, "SELECT id FROM files WHERE path='" + path + "'");
  const std::string fp =
      TextScalar(*f.db, "SELECT fingerprint FROM files WHERE id=" +
                            std::to_string(id));

  SECTION("matching fingerprint clears needs_extract") {
    ExtractResult r;
    r.file_id = id;
    r.fingerprint = fp;
    r.content = "lyrics body text";
    r.tags = "Brundlefly Telepod";
    r.meta = {{"artist", "Brundlefly"}, {"duration", "214"}};
    idx.ApplyExtraction(r);

    CHECK(Scalar(*f.db, "SELECT needs_extract FROM files WHERE id=" +
                            std::to_string(id)) == 0);
    CHECK(TextScalar(*f.db, "SELECT content FROM file_text WHERE rowid=" +
                                std::to_string(id)) == "lyrics body text");
    CHECK(Scalar(*f.db, "SELECT count(*) FROM file_meta WHERE file_id=" +
                            std::to_string(id)) == 2);
    CHECK(TextScalar(*f.db, "SELECT value FROM file_meta WHERE file_id=" +
                                std::to_string(id) + " AND key='artist'") ==
          "Brundlefly");
  }

  SECTION("stale fingerprint leaves the file flagged (race guard)") {
    ExtractResult r;
    r.file_id = id;
    r.fingerprint = fp + "-stale";  // file "changed" since the extractor read
    r.content = "whatever";
    idx.ApplyExtraction(r);

    CHECK(Scalar(*f.db, "SELECT needs_extract FROM files WHERE id=" +
                            std::to_string(id)) == 1);
  }

  SECTION("re-applying replaces prior file_meta rows") {
    ExtractResult r1;
    r1.file_id = id;
    r1.fingerprint = fp;
    r1.meta = {{"artist", "Old"}, {"album", "Gone"}};
    idx.ApplyExtraction(r1);
    REQUIRE(Scalar(*f.db, "SELECT count(*) FROM file_meta WHERE file_id=" +
                              std::to_string(id)) == 2);

    ExtractResult r2;
    r2.file_id = id;
    r2.fingerprint = fp;
    r2.meta = {{"artist", "New"}};
    idx.ApplyExtraction(r2);
    CHECK(Scalar(*f.db, "SELECT count(*) FROM file_meta WHERE file_id=" +
                            std::to_string(id)) == 1);
    CHECK(TextScalar(*f.db, "SELECT value FROM file_meta WHERE file_id=" +
                                std::to_string(id) + " AND key='artist'") ==
          "New");
  }
}
