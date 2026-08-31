#pragma once
#include <string>

namespace macassist {

// Maps a lowercase extension (no leading dot) to one of
// "document" | "image" | "audio" | "video", or "" if the type is not
// indexed in v1. This is the allow-list that keeps the index high-signal.
std::string KindForExtension(const std::string& ext);

}  // namespace macassist
