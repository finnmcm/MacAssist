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
  };
  auto it = kMap.find(ext);
  return (it == kMap.end()) ? std::string() : it->second;
}

}  // namespace macassist
