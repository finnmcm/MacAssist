#include <catch2/catch.hpp>

#include <nlohmann/json.hpp>

#include <string>

#include "index/database.hpp"
#include "ipc/request_router.hpp"
#include "macassist/protocol.hpp"
#include "sweep/sweep_status.hpp"
#include "test_util.hpp"

using namespace macassist;
using json = nlohmann::json;
using macassist::test::TempDir;

namespace {
struct RouterFixture {
  TempDir tmp;
  std::unique_ptr<Database> db;
  std::string err;
  SweepStatus status;
  ExtractStatus extract;
  RouterFixture() {
    db = Database::Open(tmp.file("index.db"), Access::ReadWrite, &err);
  }
  json handle(const std::string& req) {
    RequestRouter router(*db, status, extract);
    return json::parse(router.Handle(req));
  }
};
}  // namespace

TEST_CASE("ping returns pong echoing the id", "[router]") {
  RouterFixture f;
  REQUIRE(f.db != nullptr);
  json resp = f.handle(R"({"v":1,"type":"ping","id":7})");
  CHECK(resp["type"] == "pong");
  CHECK(resp["id"] == 7);
  CHECK(resp["v"] == kProtocolVersion);
}

TEST_CASE("status reports index and extraction counters", "[router]") {
  RouterFixture f;
  REQUIRE(f.db != nullptr);
  REQUIRE(f.db->Exec(
      "INSERT INTO files(path,name,missing,needs_extract) VALUES('/a','a',0,1)",
      &f.err));
  f.status.sweeping.store(true);
  f.extract.extracted.store(5);

  json resp = f.handle(R"({"v":1,"type":"status","id":1})");
  CHECK(resp["type"] == "status");
  CHECK(resp["indexedFiles"] == 1);
  CHECK(resp["extractedFiles"] == 5);
  CHECK(resp["pending"] == 1);
  CHECK(resp["sweeping"] == true);
  CHECK(resp.contains("afmAvailable"));
}

TEST_CASE("query returns a results frame", "[router]") {
  RouterFixture f;
  REQUIRE(f.db != nullptr);
  json resp = f.handle(R"({"v":1,"type":"query","id":3,"text":"resume"})");
  CHECK(resp["type"] == "results");
  CHECK(resp["id"] == 3);
  CHECK(resp["query"] == "resume");
  CHECK(resp["items"].is_array());  // empty index -> no items, still an array
}

TEST_CASE("malformed and unknown requests return errors", "[router]") {
  RouterFixture f;
  REQUIRE(f.db != nullptr);

  SECTION("invalid JSON") {
    CHECK(f.handle("not json at all")["type"] == "error");
  }
  SECTION("missing type field") {
    CHECK(f.handle(R"({"v":1,"id":1})")["type"] == "error");
  }
  SECTION("unknown type echoes the id") {
    json resp = f.handle(R"({"v":1,"type":"frobnicate","id":9})");
    CHECK(resp["type"] == "error");
    CHECK(resp["id"] == 9);
  }
}
