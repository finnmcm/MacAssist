#pragma once
#include <functional>
#include <string>

namespace macassist {

// A minimal blocking Unix-domain-socket server. It listens on a socket
// path and, for each accepted connection, reads length-prefixed request
// frames and writes the response frames produced by `handler`, until the
// peer disconnects. Connections are served one at a time -- enough for
// the Phase 1 skeleton; a threaded / event-loop version comes later.
class SocketServer {
 public:
  // Maps a request frame (JSON text) to a response frame (JSON text).
  using Handler = std::function<std::string(const std::string&)>;

  SocketServer(std::string socket_path, Handler handler);
  ~SocketServer();

  SocketServer(const SocketServer&) = delete;
  SocketServer& operator=(const SocketServer&) = delete;

  // Creates, binds (mode 0600), and listens on the socket. Returns false
  // (reason logged to stderr) if any step fails.
  bool Start();

  // Runs the accept loop, blocking until Stop() is requested or a fatal
  // error occurs.
  void Run();

  // Asks Run() to return. Async-signal-safe: writes one byte to a
  // self-pipe that the accept loop selects on, so it is safe to call
  // from a signal handler.
  void Stop();

 private:
  void ServeConnection(int client_fd);

  std::string socket_path_;
  Handler handler_;
  int listen_fd_ = -1;
  int stop_pipe_[2] = {-1, -1};  // [0] read end, [1] write end
};

}  // namespace macassist
