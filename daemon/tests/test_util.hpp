#pragma once
// Small helpers shared by the daemon tests: a self-cleaning temp directory,
// file writers, and a poll-with-timeout for the threaded integration tests.
#include <unistd.h>  // mkdtemp

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>

namespace macassist::test {

namespace fs = std::filesystem;

// Creates a unique temporary directory (via mkdtemp) and recursively removes it
// on destruction. Move-only.
class TempDir {
 public:
  TempDir() {
    const char* base = std::getenv("TMPDIR");
    std::string tmpl =
        (base && *base ? std::string(base) : std::string("/tmp/")) +
        "macassist_test_XXXXXX";
    std::string buf = tmpl;
    char* p = ::mkdtemp(buf.data());
    path_ = (p != nullptr) ? fs::path(p) : fs::path();
  }
  ~TempDir() {
    if (!path_.empty()) {
      std::error_code ec;
      fs::remove_all(path_, ec);
    }
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const fs::path& path() const { return path_; }
  // A child path under the temp dir (not created).
  std::string file(const std::string& name) const {
    return (path_ / name).string();
  }

 private:
  fs::path path_;
};

// Writes `content` to `path`, creating parent dirs. Returns the path.
inline std::string WriteFile(const std::string& path,
                             const std::string& content) {
  std::error_code ec;
  fs::create_directories(fs::path(path).parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  return path;
}

// Polls `pred` every 20 ms up to `timeout`; returns true as soon as it holds.
// Used by the threaded Writer/Extractor integration tests, where work lands
// asynchronously (crawl, FSEvents, extraction).
template <typename Pred>
bool WaitFor(Pred pred, std::chrono::milliseconds timeout =
                            std::chrono::seconds(15)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return pred();
}

}  // namespace macassist::test
