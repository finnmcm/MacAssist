#include <catch2/catch.hpp>

#include "index/file_kind.hpp"

using macassist::KindForExtension;

TEST_CASE("KindForExtension maps known extensions to kinds", "[file_kind]") {
  CHECK(KindForExtension("pdf") == "document");
  CHECK(KindForExtension("txt") == "document");
  CHECK(KindForExtension("md") == "document");
  CHECK(KindForExtension("csv") == "document");
  CHECK(KindForExtension("docx") == "document");

  CHECK(KindForExtension("png") == "image");
  CHECK(KindForExtension("jpg") == "image");
  CHECK(KindForExtension("heic") == "image");

  CHECK(KindForExtension("mp3") == "audio");
  CHECK(KindForExtension("m4a") == "audio");
  CHECK(KindForExtension("flac") == "audio");

  CHECK(KindForExtension("mp4") == "video");
  CHECK(KindForExtension("mov") == "video");
  CHECK(KindForExtension("mkv") == "video");

  CHECK(KindForExtension("zip") == "archive");

  // source code
  CHECK(KindForExtension("py") == "code");
  CHECK(KindForExtension("js") == "code");
  CHECK(KindForExtension("ts") == "code");
  CHECK(KindForExtension("swift") == "code");
  CHECK(KindForExtension("cpp") == "code");
  CHECK(KindForExtension("go") == "code");
  CHECK(KindForExtension("rs") == "code");
  CHECK(KindForExtension("sh") == "code");
  // config / markup are intentionally "code", not "document"
  CHECK(KindForExtension("json") == "code");
  CHECK(KindForExtension("yaml") == "code");
  CHECK(KindForExtension("xml") == "code");
  CHECK(KindForExtension("html") == "code");
  CHECK(KindForExtension("css") == "code");

  // text/markdown stay "document" -- editor-eligible app-side, not "code"
  CHECK(KindForExtension("txt") == "document");
  CHECK(KindForExtension("md") == "document");
}

TEST_CASE("KindForExtension rejects unknown and malformed input", "[file_kind]") {
  CHECK(KindForExtension("") == "");
  CHECK(KindForExtension("xyz") == "");
  CHECK(KindForExtension("exe") == "");
  // The map is lowercase-only; callers lowercase first, so an uppercase
  // extension is (by contract) not found here.
  CHECK(KindForExtension("PDF") == "");
  // A leading dot is the caller's job to strip; ".pdf" is not a key.
  CHECK(KindForExtension(".pdf") == "");
}
