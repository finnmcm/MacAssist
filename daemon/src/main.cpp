#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "index/database.hpp"
#include "ipc/request_router.hpp"
#include "ipc/socket_server.hpp"
#include "macassist/protocol.hpp"
#include "sweep/writer.hpp"

namespace {

// The running server, so the signal handler can ask it to stop. Only
// Stop() (async-signal-safe: a single self-pipe write) is called here.
macassist::SocketServer* g_server = nullptr;

void HandleSignal(int /*signo*/) {
  if (g_server != nullptr) g_server->Stop();
}

// Default indexed roots (see PLAN.md): the high-signal user folders.
std::vector<std::string> DefaultRoots() {
  const char* home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') return {};
  const std::string h = home;
  return {h + "/Desktop",  h + "/Documents", h + "/Downloads",
          h + "/Movies",   h + "/Music",     h + "/Pictures"};
}

}  // namespace

int main(int argc, char** argv) {
  std::string socket_path = macassist::DefaultSocketPath();
  std::string db_path = macassist::DefaultDbPath();
  std::vector<std::string> roots;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](const char* flag) -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "macassistd: %s needs an argument\n", flag);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--socket") {
      socket_path = next("--socket");
    } else if (a == "--db") {
      db_path = next("--db");
    } else if (a == "--root") {
      roots.push_back(next("--root"));
    } else {
      std::fprintf(stderr, "macassistd: unknown argument: %s\n", a.c_str());
      return 2;
    }
  }
  if (roots.empty()) roots = DefaultRoots();
  if (socket_path.empty() || db_path.empty()) {
    std::fprintf(stderr, "macassistd: cannot determine paths (HOME unset?)\n");
    return 1;
  }

  std::string err;
  // The writer owns the read-write connection and runs the sweep on its own
  // long-lived thread. Start() opens + migrates synchronously (so the reader
  // opens against a finished schema), then launches the background crawl.
  macassist::Writer writer(db_path, roots);
  if (!writer.Start(&err)) {
    std::fprintf(stderr, "macassistd: open index %s failed: %s\n",
                 db_path.c_str(), err.c_str());
    return 1;
  }
  std::fprintf(stderr, "macassistd: index at %s\n", db_path.c_str());

  // Read-only connection for the query path, opened after the writer created
  // the schema. WAL serves its reads concurrently with the ongoing sweep.
  auto rdb = macassist::Database::Open(db_path, macassist::Access::ReadOnly,
                                       &err);
  if (!rdb) {
    std::fprintf(stderr, "macassistd: open index (read) failed: %s\n",
                 err.c_str());
    return 1;
  }

  macassist::RequestRouter router(*rdb, writer.status());
  macassist::SocketServer server(
      socket_path,
      [&router](const std::string& req) { return router.Handle(req); });
  if (!server.Start()) {
    std::fprintf(stderr, "macassistd: failed to start on %s\n",
                 socket_path.c_str());
    return 1;
  }

  g_server = &server;
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);
  std::signal(SIGPIPE, SIG_IGN);  // report write failures via return values

  std::fprintf(stderr, "macassistd: listening on %s\n", socket_path.c_str());
  server.Run();
  std::fprintf(stderr, "macassistd: shutting down\n");
  writer.Stop();  // end the sweep promptly; the destructor joins the thread
  return 0;
}
