#include "sweep/dir_filter.hpp"

#include <cctype>
#include <filesystem>
#include <unordered_set>

namespace fs = std::filesystem;

namespace macassist {
namespace {

std::string LowerExtOf(const std::string& name) {
  const auto dot = name.rfind('.');
  if (dot == std::string::npos || dot + 1 >= name.size()) return {};
  std::string e = name.substr(dot + 1);
  for (char& c : e) c = static_cast<char>(std::tolower((unsigned char)c));
  return e;
}

}  // namespace

bool IsExcludedDir(const std::string& name) {
  static const std::unordered_set<std::string> kSkip = {
      "node_modules", ".git",   ".svn",   ".hg",    "Library",
      "DerivedData",  ".Trash", ".cache", "Caches", "Applications"};
  if (!name.empty() && name[0] == '.') return true;  // hidden directories
  return kSkip.count(name) > 0;
}

bool IsPackageBundle(const std::string& name) {
  static const std::unordered_set<std::string> kBundleExts = {
      "app", "pages", "key", "numbers", "photoslibrary", "rtfd"};
  const std::string ext = LowerExtOf(name);
  return !ext.empty() && kBundleExts.count(ext) > 0;
}

bool IsInsidePackageBundle(const std::string& path) {
  const fs::path p(path);
  // Walk components, remembering whether the *previous* one was a bundle: that
  // means the current component (and anything deeper) is inside a bundle. The
  // leaf itself never counts as "inside" -- only its ancestors do.
  bool ancestor_bundle = false;
  for (auto it = p.begin(); it != p.end(); ++it) {
    if (ancestor_bundle) return true;
    ancestor_bundle = IsPackageBundle(it->string());
  }
  return false;
}

}  // namespace macassist
