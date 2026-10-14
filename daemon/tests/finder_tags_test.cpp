#include <catch2/catch.hpp>

#include <sys/xattr.h>

#include <string>

#include <nlohmann/json.hpp>

#include "extract/finder_tags.hpp"
#include "test_util.hpp"

using namespace macassist;
using macassist::test::TempDir;
using macassist::test::WriteFile;
using json = nlohmann::json;

namespace {

// Finder stores tags as a binary plist, but NSPropertyListSerialization (what
// the bridge uses) also reads XML plists -- so the tests write XML, which is
// far easier to author than a hand-rolled binary plist.
void SetTagsXattr(const std::string& path, const std::string& plist_xml) {
  REQUIRE(setxattr(path.c_str(), "com.apple.metadata:_kMDItemUserTags",
                   plist_xml.data(), plist_xml.size(), 0, 0) == 0);
}

const char* kDocHeader =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n";

}  // namespace

TEST_CASE("ReadFinderTagsJson parses name and color", "[extract][finder_tags]") {
  TempDir tmp;
  const std::string p = WriteFile(tmp.file("a.txt"), "x");
  // A colored tag ("Red\n6") and an uncolored named tag ("Work").
  SetTagsXattr(p, std::string(kDocHeader) +
                      "<plist version=\"1.0\"><array>"
                      "<string>Red\n6</string>"
                      "<string>Work</string>"
                      "</array></plist>");

  const json tags = json::parse(ReadFinderTagsJson(p));
  REQUIRE(tags.is_array());
  REQUIRE(tags.size() == 2);
  CHECK(tags[0]["name"] == "Red");
  CHECK(tags[0]["color"] == 6);
  CHECK(tags[1]["name"] == "Work");
  CHECK(tags[1]["color"] == 0);  // named tag, no color
}

TEST_CASE("ReadFinderTagsJson returns empty for an untagged file",
          "[extract][finder_tags]") {
  TempDir tmp;
  const std::string p = WriteFile(tmp.file("b.txt"), "x");
  CHECK(ReadFinderTagsJson(p).empty());
}

TEST_CASE("ReadFinderTagsJson is empty for a missing file",
          "[extract][finder_tags]") {
  TempDir tmp;
  CHECK(ReadFinderTagsJson(tmp.file("nope.txt")).empty());
}
