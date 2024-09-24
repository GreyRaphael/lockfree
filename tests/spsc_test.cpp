#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "queue/spsc.hpp"

#include <doctest/doctest.h>

#include <format>
#include <optional>
#include <thread>

TEST_CASE("testing SPSC") {
    lockfree::SPSC<int, 1024> queue;

    std::jthread producer{[&queue] {
        for (auto i = 0; i < 10; ++i) {
            while (!queue.push(i)) {
                std::cout << "full, cannot push\n";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }};
    std::jthread consumer{[&queue] {
        while (true) {
            std::optional<int> value;
            while (!(value = queue.pop())) {
                std::cout << "empty, cannot pop\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            std::cout << std::format("consumer got {}\n", *value);
        }
    }};
}

TEST_CASE("replace modulo operations with bitmasking for faster index wrapping") {
    constexpr int N = 1024;
    auto mask = N - 1;
    for (size_t i = 1020; i < 1040; ++i) {
        CHECK((i & mask) == (i % N));
    }
}