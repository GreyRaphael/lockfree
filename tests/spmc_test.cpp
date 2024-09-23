#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "spmc.hpp"

#include <doctest/doctest.h>

#include <format>
#include <optional>
#include <thread>
#include <vector>

#include "def.hpp"

TEST_CASE("testing SPMC broadcast") {
    constexpr int NUM_CONSUMERS = 3;
    lockfree::SPMC<int, 1024, NUM_CONSUMERS, lockfree::trans::broadcast> queue;

    std::jthread producer{[&queue] {
        for (auto i = 0; i < 10; ++i) {
            while (!queue.push(i)) {
                std::cout << "full, cannot push\n";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }};

    std::vector<std::jthread> consumers;
    consumers.reserve(NUM_CONSUMERS);
    for (auto id = 0; id < NUM_CONSUMERS; ++id) {
        consumers.emplace_back([&queue, id] {
            while (true) {
                std::optional<int> value;
                // for broadcast mode, consumerId must < NUM_CONSUMERS
                while (!(value = queue.pop(id))) {
                    std::cout << "empty, cannot pop\n";
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
                std::cout << std::format("consumer{} got {}\n", id, *value);
            }
        });
    }
}

TEST_CASE("testing SPMC unicast") {
    constexpr int NUM_CONSUMERS = 3;
    // for unicast mode, NUM_CONSUMERS never take effect
    lockfree::SPMC<int, 1024, NUM_CONSUMERS, lockfree::trans::unicast> queue;

    std::jthread producer{[&queue] {
        for (auto i = 0; i < 30; ++i) {
            while (!queue.push(i)) {
                std::cout << "full, cannot push\n";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }};

    std::vector<std::jthread> consumers;
    consumers.reserve(NUM_CONSUMERS);
    for (auto id = 0; id < NUM_CONSUMERS; ++id) {
        consumers.emplace_back([&queue, id] {
            while (true) {
                std::optional<int> value;
                while (!(value = queue.pop())) {
                    std::cout << "empty, cannot pop\n";
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
                std::cout << std::format("consumer{} got {}\n", id, *value);
            }
        });
    }
}