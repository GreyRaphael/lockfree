#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "queue/mpsc.hpp"

#include <doctest/doctest.h>

#include <format>
#include <optional>
#include <thread>
#include <vector>

TEST_CASE("testing MPSC") {
    lockfree::MPSC<int, 1024> queue;

    constexpr int NUM_PRODUCTORS = 3;
    std::vector<std::jthread> consumers;
    consumers.reserve(NUM_PRODUCTORS);
    for (auto id = 0; id < NUM_PRODUCTORS; ++id) {
        consumers.emplace_back([&queue, id] {
            for (auto i = 0; i < 10; ++i) {
                while (!queue.push(i + id * 1000 + 1000)) {
                    std::cout << "full, cannot push\n";
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
    }

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