FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(asmm_tests
    test_avellaneda_stoikov.cpp
    test_volatility.cpp
    test_paper_exchange.cpp
    test_binance_parser.cpp
    test_engine.cpp)
target_link_libraries(asmm_tests PRIVATE asmm::core asmm_warnings GTest::gtest_main)
target_compile_definitions(asmm_tests PRIVATE ASMM_SAMPLE_DATA="${ASMM_SAMPLE_DATA}")

include(GoogleTest)
gtest_discover_tests(asmm_tests)
