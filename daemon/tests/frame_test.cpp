#include <catch2/catch.hpp>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>

#include "macassist/frame.hpp"

using namespace macassist;

namespace {
// A connected pair of local sockets; closes both on destruction.
struct SocketPair {
  int fd[2] = {-1, -1};
  SocketPair() { ::socketpair(AF_UNIX, SOCK_STREAM, 0, fd); }
  ~SocketPair() {
    if (fd[0] >= 0) ::close(fd[0]);
    if (fd[1] >= 0) ::close(fd[1]);
  }
};
}  // namespace

TEST_CASE("WriteFrame/ReadFrame round-trip", "[frame]") {
  SocketPair s;
  REQUIRE(s.fd[0] >= 0);

  SECTION("typical payload") {
    const std::string payload = R"({"v":1,"type":"ping","id":7})";
    REQUIRE(WriteFrame(s.fd[0], payload));
    std::string out;
    REQUIRE(ReadFrame(s.fd[1], out));
    CHECK(out == payload);
  }

  SECTION("empty payload") {
    REQUIRE(WriteFrame(s.fd[0], ""));
    std::string out = "stale";
    REQUIRE(ReadFrame(s.fd[1], out));
    CHECK(out.empty());
  }

  SECTION("payload with embedded NULs and binary bytes") {
    std::string payload("ab\0cd\x01\xff", 7);
    REQUIRE(WriteFrame(s.fd[0], payload));
    std::string out;
    REQUIRE(ReadFrame(s.fd[1], out));
    CHECK(out == payload);
    CHECK(out.size() == 7);
  }
}

TEST_CASE("WriteFrame rejects an oversize payload", "[frame]") {
  SocketPair s;
  REQUIRE(s.fd[0] >= 0);
  // Don't actually allocate 16 MiB of data; a string just over the cap.
  std::string big(kMaxFrameSize + 1, 'x');
  CHECK_FALSE(WriteFrame(s.fd[0], big));
}

TEST_CASE("ReadFrame rejects a length prefix above the cap", "[frame]") {
  SocketPair s;
  REQUIRE(s.fd[0] >= 0);
  // Hand-write a 4-byte big-endian length that exceeds kMaxFrameSize.
  uint32_t bad = htonl(kMaxFrameSize + 1);
  REQUIRE(::write(s.fd[0], &bad, sizeof(bad)) == sizeof(bad));
  std::string out;
  CHECK_FALSE(ReadFrame(s.fd[1], out));
}

TEST_CASE("ReadFrame returns false on clean EOF", "[frame]") {
  SocketPair s;
  REQUIRE(s.fd[0] >= 0);
  ::close(s.fd[0]);  // peer closes with nothing written
  s.fd[0] = -1;
  std::string out;
  CHECK_FALSE(ReadFrame(s.fd[1], out));
}
