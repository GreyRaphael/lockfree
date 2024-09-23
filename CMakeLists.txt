cmake_minimum_required(VERSION 3.20.0)
project(lockfree VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)

# Option to build tests
option(BUILD_TESTS "Build test programs" ON)

if(BUILD_TESTS)
    find_package(doctest CONFIG REQUIRED)
    enable_testing()

    # test spsc
    add_executable(spsc_test tests/spsc_test.cpp)
    target_link_libraries(spsc_test PRIVATE doctest::doctest)
    target_include_directories(spsc_test PRIVATE include)
    add_test(NAME SpscTest COMMAND spsc_test)

    # test spmc
    add_executable(spmc_test tests/spmc_test.cpp)
    target_link_libraries(spmc_test PRIVATE doctest::doctest)
    target_include_directories(spmc_test PRIVATE include)
    add_test(NAME SpmcTest COMMAND spmc_test)

    # test mpsc
    add_executable(mpsc_test tests/mpsc_test.cpp)
    target_link_libraries(mpsc_test PRIVATE doctest::doctest)
    target_include_directories(mpsc_test PRIVATE include)
    add_test(NAME MpscTest COMMAND mpsc_test)

    # # test mpmc
    # add_executable(mpmc_test tests/mpmc_test.cpp)
    # target_link_libraries(mpmc_test PRIVATE doctest::doctest)
    # add_test(NAME MpmcTest COMMAND mpmc_test)
endif()