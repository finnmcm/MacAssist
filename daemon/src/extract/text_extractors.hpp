#pragma once
#include <string>
#include <utility>
#include <vector>

namespace macassist {

// The payload the extractor thread produces for one file and the writer
// applies (FTS content/tags + typed file_meta + the staleness-clear guard).
// `fingerprint` is the value the extractor saw on the row it pulled; the
// writer clears needs_extract only if `files.fingerprint` still matches, so a
// file edited mid-extraction is left flagged and re-extracted (brick 5).
struct ExtractResult {
  long long file_id = 0;
  std::string fingerprint;
  std::string content;  // body text -> file_text.content (<=100 KB)
  std::string tags;     // flattened metadata -> file_text.tags (searchable)
  std::vector<std::pair<std::string, std::string>> meta;  // -> file_meta rows
  long long downloaded_at = 0;  // epoch secs; 0 = no download signal (not written)
};

// Each extractor fills `out.content` / `out.tags` / `out.meta` from the file at
// `path`. They never throw; on any failure (unreadable, locked, unsupported)
// they leave the outputs empty -- the caller still stamps the file done, so a
// bad file is never retried in a loop.
void ExtractPlainText(const std::string& path, ExtractResult& out);  // txt/md/csv
void ExtractPdfText(const std::string& path, ExtractResult& out);    // pdf
void ExtractMediaTags(const std::string& path, const std::string& kind,
                      ExtractResult& out);  // image / audio / video

}  // namespace macassist
