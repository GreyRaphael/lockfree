#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "threadpool.hpp"

#include <doctest/doctest.h>

TEST_CASE("lambda") {
    lockfree::threadpool<4, 1024> pool;
    auto future1 = pool.submit([](int x, int y) { return x + y; }, 100, 1000);
    CHECK_EQ(1100, future1.get());
}

int mymul(int x, int y) {
    return x * y;
}

TEST_CASE("lambda") {
    lockfree::threadpool<4, 1024> pool;
    auto future1 = pool.submit(mymul, 100, 1000);
    CHECK_EQ(100000, future1.get());
}