// The single translation unit that generates Catch2's main(). Kept separate so
// the heavy single-header is compiled only once for the whole suite.
#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
