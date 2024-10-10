#include <hv/Event.h>
#include <hv/EventLoop.h>
#include <hv/WebSocketChannel.h>
#include <hv/WebSocketServer.h>
#include <hv/http_content.h>

#include <chrono>
#include <format>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "queue/spmc.hpp"

struct MyData {
    int id;
    double value;
    char msg[16];
};

#define BUFFER_CAPACITY 128
#define MAX_READERS 16

// Thread-safe container for clients
class ClientManager {
    mutable std::shared_mutex mutex_;
    std::unordered_map<WebSocketChannelPtr, size_t> channels_;  // {channel, client_id}

   public:
    void add(WebSocketChannelPtr const& channel, size_t id) {
        std::unique_lock lock(mutex_);
        channels_[channel] = id;
    }

    void remove(WebSocketChannelPtr const& channel) {
        std::unique_lock lock(mutex_);
        channels_.erase(channel);
    }

    // Allows safe iteration over clients without copying
    void for_each_client(auto&& func) const {
        std::shared_lock lock(mutex_);
        for (const auto& [channel, client_id] : channels_) {
            func(channel, client_id);
        }
    }
};

int main(int argc, char** argv) {
    ClientManager cm;
    WebSocketService ws;  // default ping interval is disabled
    ws.onopen = [&cm](WebSocketChannelPtr const& channel, const HttpRequestPtr& req) {
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

        cm.add(channel, id);
        std::cout << std::format("client {} connected {}\n", id, req->Path());
    };
    ws.onclose = [&cm](WebSocketChannelPtr const& channel) {
        cm.remove(channel);
        std::cout << "client disconnected\n";
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

    std::jthread sender{[&cm, &queue] {
        while (true) {
            cm.for_each_client([&queue](WebSocketChannelPtr const& channel, size_t client_id) {
                std::optional<MyData> data;
                data = queue.pop(client_id);
                // if data is empty, the read_pos of lockfree queue won't be updated
                if (data.has_value()) {
                    auto ptr = reinterpret_cast<const char*>(&data.value());
                    auto ret = channel->send(ptr, sizeof(MyData));
                    // if ret < 0, send failed
                    std::cout << std::format("send {} to {}, ret={}------------\n", data.value().msg, client_id, ret);
                }
            });
        }
    }};
}
