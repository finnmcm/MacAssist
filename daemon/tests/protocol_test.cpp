#include <catch2/catch.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>

#include "macassist/protocol.hpp"
#include "test_util.hpp"

namespace fs = std::filesystem;
using namespace macassist;

// Save/restore HOME around a test that depends on it.
namespace {
struct HomeGuard {
  std::string saved;
  bool had = false;
  HomeGuard() {
    const char* h = std::getenv("HOME");
    if (h != nullptr) {
      had = true;
      saved = h;
    }
  }
  ~HomeGuard() {
    if (had) {
      ::setenv("HOME", saved.c_str(), 1);
    } else {
      ::unsetenv("HOME");
    }
  }
};
}  // namespace

TEST_CASE("Default paths follow HOME", "[protocol]") {
  HomeGuard guard;

  ::setenv("HOME", "/Users/tester", 1);
  CHECK(DefaultSocketPath() ==
        "/Users/tester/Library/Application Support/MacAssist/daemon.sock");
  CHECK(DefaultDbPath() ==
        "/Users/tester/Library/Application Support/MacAssist/index.db");

  SECTION("empty when HOME is unset") {
    ::unsetenv("HOME");
    CHECK(DefaultSocketPath().empty());
    CHECK(DefaultDbPath().empty());
  }
}

TEST_CASE("EnsureParentDir creates nested directories", "[protocol]") {
  macassist::test::TempDir tmp;
  const std::string target =
      tmp.file("a/b/c/index.db");  // parents a/b/c do not exist yet

  REQUIRE(EnsureParentDir(target));
  CHECK(fs::is_directory(fs::path(target).parent_path()));

  // Idempotent: a second call on existing dirs still succeeds.
  CHECK(EnsureParentDir(target));
}

TEST_CASE("EnsureParentDir tolerates a path with no directory component",
          "[protocol]") {
  CHECK(EnsureParentDir("index.db"));
}
