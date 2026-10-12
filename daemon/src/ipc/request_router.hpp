#pragma once
#include <string>

namespace macassist {

class Database;
struct SweepStatus;
struct ExtractStatus;

// Routes a request frame (JSON text) to a response frame (JSON text),
// querying the read-only index connection and reading live sweep progress.
// Performs no socket I/O, so it stays directly unit-testable.
class RequestRouter {
 public:
  RequestRouter(Database& db, const SweepStatus& status,
                const ExtractStatus& extract_status)
      : db_(db), status_(status), extract_status_(extract_status) {}

  std::string Handle(const std::string& request_json);

 private:
  Database& db_;
  const SweepStatus& status_;
  const ExtractStatus& extract_status_;
};

}  // namespace macassist
