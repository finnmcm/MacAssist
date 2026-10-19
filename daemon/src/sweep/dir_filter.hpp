#pragma once
#include <string>

namespace macassist {

// Directory classification shared by the crawl and the FSEvents handler, so
// both decide identically what to descend into, index, or skip.

// Directory names we never descend into or index (plus any hidden dir).
bool IsExcludedDir(const std::string& name);

// True if `name` (a single path component) is a macOS package bundle --
// .app/.pages/.key/.numbers/.photoslibrary/.rtfd. On folder-indexed roots these
// are treated as a single entry: indexed as one folder row, not descended into.
bool IsPackageBundle(const std::string& name);

// True if any *ancestor* component of `path` is a package bundle (the leaf
// itself is ignored). Used to skip a bundle's internal files on folder-indexed
// roots, matching the crawl's "don't descend into bundles" behavior.
bool IsInsidePackageBundle(const std::string& path);

}  // namespace macassist
