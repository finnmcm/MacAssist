#include "extract/text_extractors.hpp"

#include <cstdio>

namespace macassist {

// Read the first 100 KB (PLAN §3 cap) of a text file verbatim into `content`.
// Bytes are stored as-is: FTS5's unicode61 tokenizer treats invalid UTF-8 as
// token boundaries, so no decoding/validation is needed here.
void ExtractPlainText(const std::string& path, ExtractResult& out) {
  constexpr size_t kCap = 100 * 1024;
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return;
  std::string buf(kCap, '\0');
  const size_t n = std::fread(buf.data(), 1, kCap, f);
  std::fclose(f);
  buf.resize(n);
  out.content = std::move(buf);
}

}  // namespace macassist
