#include "index/file_kind.hpp"

#include <unordered_map>

namespace macassist {

std::string KindForExtension(const std::string& ext) {
  static const std::unordered_map<std::string, std::string> kMap = {
      // documents
      {"pdf", "document"},  {"doc", "document"},  {"docx", "document"},
      {"txt", "document"},  {"md", "document"},   {"rtf", "document"},
      {"pages", "document"},{"key", "document"},  {"ppt", "document"},
      {"pptx", "document"}, {"xls", "document"},  {"xlsx", "document"},
      {"csv", "document"},
      // images
      {"png", "image"},     {"jpg", "image"},     {"jpeg", "image"},
      {"heic", "image"},    {"gif", "image"},     {"webp", "image"},
      {"bmp", "image"},     {"tiff", "image"},    {"tif", "image"},
      // audio
      {"mp3", "audio"},     {"m4a", "audio"},     {"wav", "audio"},
      {"aac", "audio"},     {"flac", "audio"},    {"aiff", "audio"},
      {"aif", "audio"},
      // video
      {"mp4", "video"},     {"mov", "video"},     {"mkv", "video"},
      {"avi", "video"},     {"m4v", "video"},     {"webm", "video"},
      // archives (the file itself; contents are not indexed)
      {"zip", "archive"},
      // source code & config/markup. Classified so they get indexed at all
      // (unknown extensions are skipped in Indexer), given the "code" icon +
      // toolbar filter, and made openable in an editor (see EDITOR_PLAN.md).
      // Config/markup (json/yaml/xml/html/css/...) are intentionally "code",
      // not "document". Content is name-only in v1 (no body extraction).
      {"py", "code"},       {"pyw", "code"},      {"js", "code"},
      {"mjs", "code"},      {"cjs", "code"},      {"ts", "code"},
      {"tsx", "code"},      {"jsx", "code"},      {"swift", "code"},
      {"c", "code"},        {"h", "code"},        {"cc", "code"},
      {"cpp", "code"},      {"cxx", "code"},      {"hpp", "code"},
      {"hh", "code"},       {"m", "code"},        {"mm", "code"},
      {"java", "code"},     {"kt", "code"},       {"kts", "code"},
      {"go", "code"},       {"rs", "code"},       {"rb", "code"},
      {"php", "code"},      {"pl", "code"},       {"sh", "code"},
      {"bash", "code"},     {"zsh", "code"},      {"fish", "code"},
      {"sql", "code"},      {"html", "code"},     {"htm", "code"},
      {"css", "code"},      {"scss", "code"},     {"less", "code"},
      {"vue", "code"},      {"svelte", "code"},   {"json", "code"},
      {"yaml", "code"},     {"yml", "code"},      {"toml", "code"},
      {"ini", "code"},      {"cfg", "code"},      {"conf", "code"},
      {"xml", "code"},      {"lua", "code"},      {"r", "code"},
      {"scala", "code"},    {"clj", "code"},      {"ex", "code"},
      {"exs", "code"},      {"erl", "code"},      {"hs", "code"},
      {"dart", "code"},     {"gradle", "code"},   {"cmake", "code"},
      {"proto", "code"},    {"graphql", "code"},  {"gql", "code"},
  };
  auto it = kMap.find(ext);
  return (it == kMap.end()) ? std::string() : it->second;
}

}  // namespace macassist
