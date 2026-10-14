#pragma once
#include <string>

namespace macassist {

// Reads the file's Finder tags (the com.apple.metadata:_kMDItemUserTags
// extended attribute) and returns a compact JSON array like
// [{"name":"Red","color":6}], or "" when the file has no tags.
//
// color is the Finder label index: 0 none, 1 gray, 2 green, 3 purple,
// 4 blue, 5 yellow, 6 red, 7 orange. A named tag with no assigned color
// reports color 0. Never throws; returns "" on any read/parse failure.
//
// Unlike content extraction, this is read on the writer thread during the
// stat/upsert pass: setting a Finder tag changes neither mtime nor size, so
// the fingerprint can't detect it -- the only reliable place to pick tags up
// is every time we stat the file.
std::string ReadFinderTagsJson(const std::string& path);

}  // namespace macassist
