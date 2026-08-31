#pragma once
#include <string>

namespace macassist {

class Database;

// Routes a request frame (JSON text) to a response frame (JSON text),
// querying the index it holds a reference to. Performs no socket I/O, so
// it stays directly unit-testable. Replaces the Phase 1 free-function
// handler now that requests need access to the database.
class RequestRouter {
 public:
  explicit RequestRouter(Database& db) : db_(db) {}

  std::string Handle(const std::string& request_json);

 private:
  Database& db_;
};

}  // namespace macassist
