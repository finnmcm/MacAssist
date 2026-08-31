#pragma once
#include <string>

namespace macassist {

// Wire-protocol version. Bump when the JSON message shapes change
// incompatibly; both sides compare this against the `v` field so a
// mismatch can be reported instead of silently misparsed.
inline constexpr int kProtocolVersion = 1;

// Values of the `type` field. Requests are app -> daemon; the rest are
// daemon -> app responses. See PLAN.md section 6 for the full table.
namespace msg {
inline constexpr const char* kQuery = "query";      // request
inline constexpr const char* kResults = "results";  // response
inline constexpr const char* kStatus = "status";    // request + response
inline constexpr const char* kAction = "action";    // request
inline constexpr const char* kReindex = "reindex";  // request
inline constexpr const char* kPing = "ping";        // request
inline constexpr const char* kPong = "pong";        // response
inline constexpr const char* kError = "error";      // response
}  // namespace msg

// Absolute path to the daemon's Unix domain socket, under
// ~/Library/Application Support/MacAssist/. Returns "" if HOME is unset.
// Does not create the parent directory (see EnsureParentDir).
std::string DefaultSocketPath();

// Absolute path to the index database, alongside the socket. Returns ""
// if HOME is unset. Does not create the parent directory.
std::string DefaultDbPath();

// Creates the parent directory of `path` and any missing intermediates
// (mkdir -p, mode 0700). Returns true if the directory exists afterward.
bool EnsureParentDir(const std::string& path);

}  // namespace macassist
