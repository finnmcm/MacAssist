#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include <nlohmann/json.hpp>

#include "macassist/frame.hpp"
#include "macassist/protocol.hpp"

using json = nlohmann::json;

namespace {

// Opens a connected client socket to the daemon at `path`, or -1.
int ConnectTo(const std::string& path) {
  sockaddr_un addr{};
  if (path.size() >= sizeof(addr.sun_path)) {
    std::fprintf(stderr, "socket path too long: %s\n", path.c_str());
    return -1;
  }
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    std::perror("socket");
    return -1;
  }
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    std::fprintf(stderr, "connect(%s): %s\n", path.c_str(),
                 std::strerror(errno));
    ::close(fd);
    return -1;
  }
  return fd;
}

void PrintUsage() {
  std::fprintf(stderr,
               "usage: mactl [--socket PATH] <command>\n"
               "  ping                 liveness check\n"
               "  status               index / sweep status\n"
               "  query <text...>      run a search\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::string socket_path = macassist::DefaultSocketPath();

  int i = 1;
  if (i < argc && std::string(argv[i]) == "--socket") {
    if (i + 1 >= argc) {
      PrintUsage();
      return 2;
    }
    socket_path = argv[i + 1];
    i += 2;
  }
  if (i >= argc) {
    PrintUsage();
    return 2;
  }
  const std::string cmd = argv[i++];

  // Build the request frame for the chosen command.
  json req;
  req["v"] = macassist::kProtocolVersion;
  req["id"] = 1;
  if (cmd == "ping") {
    req["type"] = macassist::msg::kPing;
  } else if (cmd == "status") {
    req["type"] = macassist::msg::kStatus;
  } else if (cmd == "query") {
    std::string text;
    for (; i < argc; ++i) {
      if (!text.empty()) text += ' ';
      text += argv[i];
    }
    if (text.empty()) {
      std::fprintf(stderr, "query: missing search text\n");
      return 2;
    }
    req["type"] = macassist::msg::kQuery;
    req["text"] = text;
    req["stage"] = "full";
  } else {
    PrintUsage();
    return 2;
  }

  if (socket_path.empty()) {
    std::fprintf(stderr, "cannot determine socket path (HOME unset?)\n");
    return 1;
  }
  int fd = ConnectTo(socket_path);
  if (fd < 0) return 1;

  int rc = 0;
  if (!macassist::WriteFrame(fd, req.dump())) {
    std::fprintf(stderr, "failed to send request\n");
    rc = 1;
  } else {
    std::string resp;
    if (!macassist::ReadFrame(fd, resp)) {
      std::fprintf(stderr, "no/invalid response from daemon\n");
      rc = 1;
    } else {
      // Pretty-print the JSON reply, falling back to raw text.
      try {
        std::printf("%s\n", json::parse(resp).dump(2).c_str());
      } catch (const json::exception&) {
        std::printf("%s\n", resp.c_str());
      }
    }
  }
  ::close(fd);
  return rc;
}
