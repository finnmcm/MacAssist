#include "macassist/frame.hpp"

#include <arpa/inet.h>  // htonl, ntohl
#include <unistd.h>     // read, write

#include <cerrno>
#include <cstddef>

namespace macassist {
namespace {

// Writes exactly `len` bytes, resuming on partial writes and EINTR.
bool WriteAll(int fd, const char* buf, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = ::write(fd, buf + off, len - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    off += static_cast<size_t>(n);
  }
  return true;
}

// Reads exactly `len` bytes; false on EOF-before-len or error.
bool ReadAll(int fd, char* buf, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = ::read(fd, buf + off, len - off);
    if (n == 0) return false;  // peer closed
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    off += static_cast<size_t>(n);
  }
  return true;
}

}  // namespace

bool WriteFrame(int fd, const std::string& payload) {
  if (payload.size() > kMaxFrameSize) return false;
  uint32_t len = htonl(static_cast<uint32_t>(payload.size()));
  if (!WriteAll(fd, reinterpret_cast<const char*>(&len), sizeof(len))) {
    return false;
  }
  return WriteAll(fd, payload.data(), payload.size());
}

bool ReadFrame(int fd, std::string& out) {
  uint32_t len_net = 0;
  if (!ReadAll(fd, reinterpret_cast<char*>(&len_net), sizeof(len_net))) {
    return false;
  }
  uint32_t len = ntohl(len_net);
  if (len > kMaxFrameSize) return false;
  out.resize(len);
  if (len == 0) return true;
  return ReadAll(fd, out.data(), len);
}

}  // namespace macassist
