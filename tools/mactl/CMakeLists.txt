# mactl: command-line client for the daemon. Speaks the same socket
# protocol the UI app will use -- the reference client and test harness.
add_executable(mactl src/main.cpp)

target_link_libraries(mactl PRIVATE macassist_shared nlohmann_json)
