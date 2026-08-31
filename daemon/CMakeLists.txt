# macassistd: the background daemon -- owns the index, sweeps the roots,
# and answers requests over the Unix socket.
add_executable(macassistd
  src/main.cpp
  src/ipc/socket_server.cpp
  src/ipc/request_router.cpp
  src/index/database.cpp
  src/index/file_kind.cpp
  src/sweep/crawler.cpp
  src/query/query_engine.cpp)

# Allows includes like "ipc/socket_server.hpp" and "index/database.hpp".
target_include_directories(macassistd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(macassistd PRIVATE
  macassist_shared nlohmann_json sqlite3)
