cmake_minimum_required(VERSION 3.20)
project(MacAssist LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emit compile_commands.json so clangd / editors resolve includes and the
# language standard correctly.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# Header-only third-party: nlohmann/json lives at third_party/nlohmann/json.hpp,
# included as <nlohmann/json.hpp>.
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE ${CMAKE_SOURCE_DIR}/third_party)

# Vendored SQLite amalgamation, built with FTS5 enabled. Compiled as its
# own target with warnings silenced (-w) so our -Wall/-Wpedantic flags
# don't drown us in third-party noise.
add_library(sqlite3 STATIC third_party/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${CMAKE_SOURCE_DIR}/third_party/sqlite)
target_compile_definitions(sqlite3 PRIVATE
  SQLITE_ENABLE_FTS5
  SQLITE_THREADSAFE=1
  SQLITE_DQS=0)
target_compile_options(sqlite3 PRIVATE -w)

add_subdirectory(shared)
add_subdirectory(daemon)
add_subdirectory(tools/mactl)
