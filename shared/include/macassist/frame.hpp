#pragma once
#include <cstdint>
#include <string>

namespace macassist {

// Upper bound on an accepted frame payload, guarding against a bad or
// hostile length prefix. 16 MiB is far above any legitimate message.
inline constexpr uint32_t kMaxFrameSize = 16u * 1024u * 1024u;

// Writes `payload` to `fd` as one length-prefixed frame: a 4-byte
// big-endian (network order) length followed by the payload bytes.
// Resumes on partial writes and EINTR. Returns false on error or if
// the payload exceeds kMaxFrameSize.
bool WriteFrame(int fd, const std::string& payload);

// Reads exactly one length-prefixed frame from `fd` into `out`.
// Returns false on clean EOF (peer closed), I/O error, or a declared
// length exceeding kMaxFrameSize.
bool ReadFrame(int fd, std::string& out);

}  // namespace macassist
