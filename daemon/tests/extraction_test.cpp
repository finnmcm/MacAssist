#include <catch2/catch.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "index/database.hpp"
#include "query/query_engine.hpp"
#include "sweep/writer.hpp"
#include "test_util.hpp"

namespace fs = std::filesystem;
using namespace macassist;
using macassist::test::TempDir;
using macassist::test::WaitFor;
using macassist::test::WriteFile;

namespace {

// True if a query for `term` returns a hit named `name` (read-only connection).
bool Finds(Database& db, const std::string& term, const std::string& name) {
  for (const auto& h : SearchFiles(db, term, 10)) {
    if (h.name == name) return true;
  }
  return false;
}

// Copy a committed fixture into the corpus if it exists; returns whether it did.
bool CopyFixture(const std::string& fixture_name, const fs::path& dest) {
  const fs::path src = fs::path(MACASSIST_FIXTURES_DIR) / fixture_name;
  std::error_code ec;
  if (!fs::exists(src, ec)) return false;
  fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
  return !ec;
}

}  // namespace

TEST_CASE("Writer + Extractor index and extract content end-to-end",
          "[extraction][integration]") {
  TempDir tmp;
  const fs::path corpus = tmp.path() / "corpus";
  fs::create_directories(corpus);

  // Text bodies with tokens that appear in NO filename, so a hit proves the
  // body was extracted (not merely a name match).
  WriteFile((corpus / "notes.txt").string(), "alpha zxqvfox omega\n");
  WriteFile((corpus / "readme.md").string(), "# Title\nbody has plumbus here\n");
  int expected = 2;

  // Optional golden fixtures exercise the Apple-framework extractors.
  const bool has_pdf = CopyFixture("report.pdf", corpus / "report.pdf");
  const bool has_jpg = CopyFixture("camera.jpg", corpus / "camera.jpg");
  const bool has_m4a = CopyFixture("song.m4a", corpus / "song.m4a");
  expected += (has_pdf ? 1 : 0) + (has_jpg ? 1 : 0) + (has_m4a ? 1 : 0);

  const std::string db_path = tmp.file("index.db");
  Writer writer(db_path, {corpus.string()}, /*folder_roots=*/{},
                std::chrono::seconds(2));
  std::string err;
  REQUIRE(writer.Start(&err));

  // Separate read-only connection, like the real query path.
  auto ro = Database::Open(db_path, Access::ReadOnly, &err);
  REQUIRE(ro != nullptr);

  // Wait for the crawl to index everything and extraction to drain.
  REQUIRE(WaitFor([&] {
    return ro->CountFiles() == expected && ro->CountPending() == 0;
  }));

  // Body-text extraction (tokens absent from filenames).
  CHECK(Finds(*ro, "zxqvfox", "notes.txt"));
  CHECK(Finds(*ro, "plumbus", "readme.md"));

  if (has_pdf) CHECK(Finds(*ro, "grimblefarben", "report.pdf"));
  if (has_jpg) CHECK(Finds(*ro, "Nimbus9000", "camera.jpg"));
  if (has_m4a) CHECK(Finds(*ro, "Brundlefly", "song.m4a"));

  writer.Stop();
}

TEST_CASE("Editing a file re-extracts its new content", "[extraction][integration]") {
  TempDir tmp;
  const fs::path corpus = tmp.path() / "corpus";
  fs::create_directories(corpus);
  const std::string note = (corpus / "note.txt").string();
  WriteFile(note, "original kryptonite body\n");

  const std::string db_path = tmp.file("index.db");
  Writer writer(db_path, {corpus.string()}, /*folder_roots=*/{},
                std::chrono::seconds(2));
  std::string err;
  REQUIRE(writer.Start(&err));
  auto ro = Database::Open(db_path, Access::ReadOnly, &err);
  REQUIRE(ro != nullptr);

  REQUIRE(WaitFor(
      [&] { return ro->CountFiles() == 1 && ro->CountPending() == 0; }));
  REQUIRE(Finds(*ro, "kryptonite", "note.txt"));

  // Rewrite with a new body token. The fingerprint is mtime(sec):size, so make
  // the content a clearly different length and let the clock tick past the
  // original write, guaranteeing the change is detectable. FSEvents (or the 2 s
  // reconcile backstop) then re-examines it and re-flags extraction.
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  WriteFile(note, "rewritten body now mentions wumbology and is clearly longer\n");

  CHECK(WaitFor([&] { return Finds(*ro, "wumbology", "note.txt"); }));
  // The old body token should no longer match (content was replaced).
  CHECK(WaitFor([&] { return !Finds(*ro, "kryptonite", "note.txt"); }));

  writer.Stop();
}
