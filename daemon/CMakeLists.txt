# macassistd: the background daemon -- owns the index, sweeps the roots,
# and answers requests over the Unix socket.
#
# The daemon's logic lives in a static library (macassistd_core) so both the
# executable and the test suite link the exact same code; main.cpp is a thin
# shell on top of it.
add_library(macassistd_core STATIC
  src/ipc/socket_server.cpp
  src/ipc/request_router.cpp
  src/index/database.cpp
  src/index/file_kind.cpp
  src/sweep/crawler.cpp
  src/sweep/dir_filter.cpp
  src/sweep/indexer.cpp
  src/sweep/fs_watcher.mm
  src/sweep/politeness.mm
  src/sweep/writer.cpp
  src/extract/extractor.cpp
  src/extract/plain_text.cpp
  src/extract/pdf_text.mm
  src/extract/media_tags.mm
  src/extract/finder_tags.mm
  src/extract/download_date.mm
  src/query/query_engine.cpp)

# PUBLIC so dependents (macassistd, daemon_tests) inherit the include path,
# the third-party libs, and the Apple frameworks.
target_include_directories(macassistd_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# The Objective-C++ bridges (FSEvents, thermal/power, PDF/media extraction)
# compile with ARC.
set_source_files_properties(
  src/sweep/fs_watcher.mm src/sweep/politeness.mm
  src/extract/pdf_text.mm src/extract/media_tags.mm
  src/extract/finder_tags.mm src/extract/download_date.mm
  PROPERTIES COMPILE_FLAGS "-fobjc-arc")

target_link_libraries(macassistd_core PUBLIC
  macassist_shared nlohmann_json sqlite3
  "-framework CoreServices" "-framework CoreFoundation"
  "-framework Foundation" "-framework IOKit"
  # Content extraction: PDF text, image (ImageIO needs CoreGraphics) and
  # audio/video metadata.
  "-framework PDFKit" "-framework ImageIO" "-framework CoreGraphics"
  "-framework AVFoundation" "-framework CoreMedia")

add_executable(macassistd src/main.cpp)
target_link_libraries(macassistd PRIVATE macassistd_core)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
