#include "ipc/socket_server.hpp"

#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>  // chmod
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <utility>

#include "macassist/frame.hpp"
#include "macassist/protocol.hpp"

namespace macassist {

SocketServer::SocketServer(std::string socket_path, Handler handler)
    : socket_path_(std::move(socket_path)), handler_(std::move(handler)) {}

SocketServer::~SocketServer() {
  if (listen_fd_ >= 0) ::close(listen_fd_);
  if (stop_pipe_[0] >= 0) ::close(stop_pipe_[0]);
  if (stop_pipe_[1] >= 0) ::close(stop_pipe_[1]);
  if (!socket_path_.empty()) ::unlink(socket_path_.c_str());
}

bool SocketServer::Start() {
  sockaddr_un addr{};
  if (socket_path_.size() >= sizeof(addr.sun_path)) {
    std::fprintf(stderr, "socket path too long: %s\n", socket_path_.c_str());
    return false;
  }
  if (!EnsureParentDir(socket_path_)) {
    std::fprintf(stderr, "cannot create socket directory for %s\n",
                 socket_path_.c_str());
    return false;
  }
  // Clear a stale socket left by a previous run before binding.
  ::unlink(socket_path_.c_str());

  listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    std::perror("socket");
    return false;
  }

  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    std::perror("bind");
    return false;
  }
  // User-only access -- this is the security boundary for the socket.
  if (::chmod(socket_path_.c_str(), 0600) != 0) {
    std::perror("chmod");
  }
  if (::listen(listen_fd_, 16) < 0) {
    std::perror("listen");
    return false;
  }
  if (::pipe(stop_pipe_) < 0) {
    std::perror("pipe");
    return false;
  }
  return true;
}

void SocketServer::Stop() {
  // Async-signal-safe wakeup: a single byte to the self-pipe unblocks
  // the select() in Run(). Best-effort; ignore the result.
  if (stop_pipe_[1] >= 0) {
    const char b = 1;
    ssize_t n = ::write(stop_pipe_[1], &b, 1);
    (void)n;
  }
}

void SocketServer::Run() {
  for (;;) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(listen_fd_, &rfds);
    FD_SET(stop_pipe_[0], &rfds);
    const int maxfd = listen_fd_ > stop_pipe_[0] ? listen_fd_ : stop_pipe_[0];

    if (::select(maxfd + 1, &rfds, nullptr, nullptr, nullptr) < 0) {
      if (errno == EINTR) continue;
      std::perror("select");
      return;
    }
    if (FD_ISSET(stop_pipe_[0], &rfds)) {
      return;  // Stop() was requested
    }
    if (FD_ISSET(listen_fd_, &rfds)) {
      const int client_fd = ::accept(listen_fd_, nullptr, nullptr);
      if (client_fd < 0) {
        if (errno == EINTR || errno == ECONNABORTED) continue;
        std::perror("accept");
        continue;
      }
      // Phase 1: serve this connection to completion on the accept
      // thread. When coexisting clients or slow (AFM) queries arrive,
      // spawn a detached thread here instead -- ServeConnection is
      // already self-contained and the index handles its own locking.
      ServeConnection(client_fd);
      ::close(client_fd);
    }
  }
}

void SocketServer::ServeConnection(int client_fd) {
  // A connection may carry multiple request frames (a long-lived client)
  // or exactly one (mactl). Loop until the peer closes or errors.
  std::string request;
  while (ReadFrame(client_fd, request)) {
    const std::string response = handler_(request);
    if (!WriteFrame(client_fd, response)) break;
  }
}

}  // namespace macassist
