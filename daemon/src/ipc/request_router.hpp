#pragma once
#include <string>

namespace macassist {

class Database;
struct SweepStatus;

// Routes a request frame (JSON text) to a response frame (JSON text),
// querying the read-only index connection and reading live sweep progress.
// Performs no socket I/O, so it stays directly unit-testable.
class RequestRouter {
 public:
  RequestRouter(Database& db, const SweepStatus& status)
      : db_(db), status_(status) {}

  std::string Handle(const std::string& request_json);

 private:
  Database& db_;
  const SweepStatus& status_;
};

}  // namespace macassist
