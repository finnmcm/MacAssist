#include "macassist/protocol.hpp"

#include <sys/stat.h>  // mkdir

#include <cerrno>
#include <cstdlib>  // getenv

namespace macassist {

std::string DefaultSocketPath() {
  const char* home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') return {};
  return std::string(home) +
         "/Library/Application Support/MacAssist/daemon.sock";
}

std::string DefaultDbPath() {
  const char* home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') return {};
  return std::string(home) +
         "/Library/Application Support/MacAssist/index.db";
}

bool EnsureParentDir(const std::string& path) {
  auto slash = path.find_last_of('/');
  if (slash == std::string::npos) return true;  // no directory component
  const std::string dir = path.substr(0, slash);

  // Create each component in turn (mkdir -p). `partial` accumulates the
  // prefix up to and including each '/'.
  std::string partial;
  for (size_t i = 0; i < dir.size(); ++i) {
    partial.push_back(dir[i]);
    const bool at_end = (i + 1 == dir.size());
    if (dir[i] == '/' || at_end) {
      if (partial == "/") continue;  // filesystem root always exists
      if (::mkdir(partial.c_str(), 0700) != 0 && errno != EEXIST) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace macassist
