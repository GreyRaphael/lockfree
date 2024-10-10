#include <hv/Event.h>
#include <hv/EventLoop.h>
#include <hv/WebSocketChannel.h>
#include <hv/WebSocketServer.h>
#include <hv/http_content.h>

#include <array>
#include <atomic>
#include <chrono>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "queue/spmc.hpp"

struct MyData {
    int id;
    double value;
    char msg[16];
};

#define BUFFER_CAPACITY 128
#define MAX_READERS 16

int main(int argc, char** argv) {
    std::array<std::atomic<WebSocketChannelPtr>, MAX_READERS> channels;  // all nullptr
    WebSocketService ws;                                                 // default ping interval is disabled
    ws.onopen = [&channels](WebSocketChannelPtr const& channel, const HttpRequestPtr& req) {
        auto id_str = req->GetParam("id", "0");
        size_t id = 0;  // default value if parsing fails
        std::from_chars(id_str.data(), id_str.data() + id_str.size(), id);

        if (id >= MAX_READERS) {
            MyData myData{};
            snprintf(myData.msg, sizeof(myData.msg), "err,id>=%u", MAX_READERS);
            channel->send(reinterpret_cast<const char*>(&myData), sizeof(MyData));
            channel->close();  // invalid id, close connection
            return;
        }

        WebSocketChannelPtr expected{nullptr};
        if (channels[id].compare_exchange_strong(expected, channel, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            // compare contained with expected, if true, set contained to channel
            std::cout << std::format("client {} connected {}\n", id, req->Path());
            // set context for channel
            channel->setContextPtr(std::make_shared<std::optional<size_t>>(id));
        } else {
            // compare contained with expected, if false, set expected to contained
            MyData myData{};
            snprintf(myData.msg, sizeof(myData.msg), "err,id=%lu in use", id);
            channel->send(reinterpret_cast<const char*>(&myData), sizeof(MyData));
            channel->setContextPtr(std::make_shared<std::optional<size_t>>(std::nullopt));
            channel->close();
            return;
        }
    };
    ws.onclose = [&channels](WebSocketChannelPtr const& channel) {
        auto id_ptr = channel->getContextPtr<std::optional<size_t>>();
        auto opt_id = *id_ptr;
        if (opt_id.has_value()) {
            channels[opt_id.value()].store(nullptr, std::memory_order_release);
            std::cout << std::format("client {} disconnected\n", opt_id.value());
        } else {
            std::cout << "client duplicated disconnected\n";
        }
    };

    hv::WebSocketServer server{&ws};
    server.setHost("localhost");
    server.setPort(8888);
    std::cout << std::format("listening to {}:{}...\n", server.host, server.port);
    server.start();

    lockfree::SPMC<MyData, BUFFER_CAPACITY, MAX_READERS, lockfree::trans::broadcast> queue;
    std::jthread writer{[&queue] {
        size_t index = 0;
        while (true) {
            MyData myData{};
            myData.id = index;
            myData.value = index * 0.1;
            snprintf(myData.msg, sizeof(myData.msg), "Data%zu", index);

            // Attempt to push data into the ring buffer
            while (!queue.push(myData)) {
                std::cout << "Queue is full, cannot push. Retrying...\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            std::cout << std::format("Writer wrote: id={}, value={:.2f}, msg={}\n", myData.id, myData.value, myData.msg);
            ++index;

            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }};

    std::jthread sender{[&channels, &queue] {
        while (true) {
            bool has_data = false;
            for (size_t i = 0; i < MAX_READERS; ++i) {
                auto channel = channels[i].load(std::memory_order_acquire);
                if (channel) {
                    auto data = queue.pop(i);
                    if (data.has_value()) {
                        auto ptr = reinterpret_cast<const char*>(&data.value());
                        auto ret = channel->send(ptr, sizeof(MyData));
                        // if ret < 0, send failed
                        std::cout << std::format("send {} to {}, ret={}------------\n", data.value().msg, i, ret);
                        has_data = true;
                    }
                }
            }

            // all not has_data
            if (!has_data) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }};
}