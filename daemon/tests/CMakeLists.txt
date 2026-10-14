# The Catch2 main is compiled once into its own TU (the single header is ~18k
# lines); every test executable links it.
add_library(catch2_main STATIC catch_main.cpp)
target_link_libraries(catch2_main PUBLIC catch2)

# One test binary over the whole daemon core. Catch prints per-assertion detail;
# CTest tracks the overall pass/fail. Run `./build/daemon/tests/daemon_tests` for
# the full report, or `ctest --test-dir build` for the summary.
add_executable(daemon_tests
  file_kind_test.cpp
  database_test.cpp
  indexer_test.cpp
  crawler_test.cpp
  query_engine_test.cpp
  plain_text_test.cpp
  frame_test.cpp
  protocol_test.cpp
  request_router_test.cpp
  extraction_test.cpp)
target_link_libraries(daemon_tests PRIVATE macassistd_core catch2_main)
target_compile_definitions(daemon_tests PRIVATE
  MACASSIST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

add_test(NAME daemon_tests COMMAND daemon_tests)
