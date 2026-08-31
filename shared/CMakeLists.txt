# Shared library: wire protocol + framing, linked by both the daemon and
# the mactl client so they cannot drift out of sync.
add_library(macassist_shared STATIC
  src/frame.cpp
  src/protocol.cpp)

target_include_directories(macassist_shared PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include)
