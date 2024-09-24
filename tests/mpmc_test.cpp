#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "queue/mpmc.hpp"

#include <doctest/doctest.h>

#include <format>
#include <optional>
#include <thread>
#include <vector>

TEST_CASE("testing MPMC unicast") {
    constexpr int NUM_PRODUCERS = 2;
    constexpr int NUM_CONSUMERS = 3;
    // for unicast mode, NUM_CONSUMERS never take effect
    lockfree::MPMC<int, 1024, NUM_CONSUMERS, lockfree::trans::unicast> queue;

    std::vector<std::jthread> producers;
    producers.reserve(NUM_PRODUCERS);
    for (size_t id = 0; id < NUM_PRODUCERS; ++id) {
        producers.emplace_back([&queue, id] {
            for (auto i = 0; i < 10; ++i) {
                while (!queue.push(i + id * 1000 + 1000)) {
                    std::cout << "full, cannot push\n";
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
    }

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

TEST_CASE("testing MPMC broadcast") {
    constexpr int NUM_PRODUCERS = 2;
    constexpr int NUM_CONSUMERS = 3;
    lockfree::MPMC<int, 1024, NUM_CONSUMERS, lockfree::trans::broadcast> queue;

    std::vector<std::jthread> producers;
    producers.reserve(NUM_PRODUCERS);
    for (size_t id = 0; id < NUM_PRODUCERS; ++id) {
        producers.emplace_back([&queue, id] {
            for (auto i = 0; i < 10; ++i) {
                while (!queue.push(i + id * 1000 + 1000)) {
                    std::cout << "full, cannot push\n";
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
    }

    std::vector<std::jthread> consumers;
    consumers.reserve(NUM_CONSUMERS);
    for (auto id = 0; id < NUM_CONSUMERS; ++id) {
        consumers.emplace_back([&queue, id] {
            while (true) {
                std::optional<int> value;
                // for broadcast mode, NUM_CONSUMERS never take effect
                while (!(value = queue.pop(id))) {
                    std::cout << "empty, cannot pop\n";
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
                std::cout << std::format("consumer{} got {}\n", id, *value);
            }
        });
    }
}