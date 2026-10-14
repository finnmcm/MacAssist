#include <catch2/catch.hpp>

#include <string>

#include "extract/text_extractors.hpp"
#include "test_util.hpp"

using namespace macassist;
using macassist::test::TempDir;
using macassist::test::WriteFile;

TEST_CASE("ExtractPlainText reads file contents", "[extract][plain_text]") {
  TempDir tmp;
  const std::string body = "the quick brown fox\nsecond line\n";
  WriteFile(tmp.file("notes.txt"), body);

  ExtractResult r;
  ExtractPlainText(tmp.file("notes.txt"), r);
  CHECK(r.content == body);
  CHECK(r.tags.empty());
  CHECK(r.meta.empty());
}

TEST_CASE("ExtractPlainText caps at 100 KB", "[extract][plain_text]") {
  TempDir tmp;
  const size_t kCap = 100 * 1024;
  std::string big(kCap + 5000, 'a');
  WriteFile(tmp.file("big.txt"), big);

  ExtractResult r;
  ExtractPlainText(tmp.file("big.txt"), r);
  CHECK(r.content.size() == kCap);
}

TEST_CASE("ExtractPlainText on a missing file yields empty, no crash",
          "[extract][plain_text]") {
  TempDir tmp;
  ExtractResult r;
  ExtractPlainText(tmp.file("does-not-exist.txt"), r);
  CHECK(r.content.empty());
}

TEST_CASE("ExtractPlainText preserves binary bytes", "[extract][plain_text]") {
  TempDir tmp;
  std::string bytes("a\0b\x01\xfe\xff" "c", 7);  // split to end the \xff escape
  WriteFile(tmp.file("bin.txt"), bytes);

  ExtractResult r;
  ExtractPlainText(tmp.file("bin.txt"), r);
  CHECK(r.content == bytes);
  CHECK(r.content.size() == 7);
}
