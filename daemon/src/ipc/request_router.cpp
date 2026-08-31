#include "ipc/request_router.hpp"

#include <nlohmann/json.hpp>

#include "index/database.hpp"
#include "macassist/protocol.hpp"
#include "query/query_engine.hpp"

namespace macassist {

using json = nlohmann::json;

namespace {

json MakeError(const std::string& message, const json& id = nullptr) {
  return json{{"v", kProtocolVersion},
              {"type", msg::kError},
              {"id", id},
              {"message", message}};
}

}  // namespace

std::string RequestRouter::Handle(const std::string& request_json) {
  json req;
  try {
    req = json::parse(request_json);
  } catch (const json::exception& e) {
    return MakeError(std::string("invalid JSON: ") + e.what()).dump();
  }

  if (!req.is_object() || !req.contains("type") || !req["type"].is_string()) {
    return MakeError("missing or non-string 'type'").dump();
  }

  const std::string type = req["type"].get<std::string>();
  const json id = req.value("id", json(nullptr));

  if (type == msg::kPing) {
    return json{{"v", kProtocolVersion}, {"type", msg::kPong}, {"id", id}}
        .dump();
  }

  if (type == msg::kStatus) {
    return json{{"v", kProtocolVersion},
                {"type", msg::kStatus},
                {"id", id},
                {"indexedFiles", db_.CountFiles()},
                {"sweeping", false},
                {"afmAvailable", false}}
        .dump();
  }

  if (type == msg::kQuery) {
    const std::string text = req.value("text", std::string());
    const std::string stage = req.value("stage", std::string("full"));
    const int limit = req.value("limit", 5);

    json items = json::array();
    for (const FileHit& h : SearchFiles(db_, text, limit)) {
      items.push_back({{"fileId", h.id},
                       {"path", h.path},
                       {"name", h.name},
                       {"kind", h.kind},
                       {"score", h.score},
                       {"modifiedAt", h.modified_at}});
    }
    return json{{"v", kProtocolVersion},
                {"type", msg::kResults},
                {"id", id},
                {"stage", stage},
                {"query", text},
                {"items", items}}
        .dump();
  }

  return MakeError("unknown type: " + type, id).dump();
}

}  // namespace macassist
