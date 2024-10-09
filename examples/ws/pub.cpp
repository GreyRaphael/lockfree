#include <hv/Event.h>
#include <hv/EventLoop.h>
#include <hv/WebSocketChannel.h>
#include <hv/WebSocketServer.h>
#include <hv/http_content.h>

#include <chrono>
#include <cstddef>
#include <format>
#include <fstream>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

#include "queue/spmc.hpp"

// Thread-safe container for clients
class ClientManager {
    mutable std::shared_mutex mutex_;
    std::unordered_map<WebSocketChannelPtr, size_t> channels_;  // {channel, client_id}
    std::unordered_map<std::string, size_t> clients_map_;       // {name, client_id}

   public:
    void add(WebSocketChannelPtr const& channel, std::string const& name) {
        std::unique_lock lock(mutex_);
        channels_[channel] = retrive_id(name);
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

    size_t retrive_id(std::string const& name) {
        if (clients_map_.contains(name)) {
            return clients_map_[name];
        }
        return SIZE_MAX;
    }

    size_t retrive_id(WebSocketChannelPtr const& channel) {
        if (channels_.contains(channel)) {
            return channels_[channel];
        }
        return SIZE_MAX;
    }

    // load registered clients from json file
    void load_clients() {
        std::ifstream fin{"clientdb.json"};
        auto j = hv::Json::parse(fin);
        for (auto&& client : j["clients"]) {
            std::string name = client["name"];
            size_t id = client["id"];
            clients_map_[name] = id;
        }
    }
};

struct MyData {
    int id;
    double value;
    char name[16];
};

#define BUFFER_CAPACITY 128  // Number of entries in the buffer
#define MAX_READERS 16

int main(int argc, char** argv) {
    std::ifstream fin{"server.json"};
    auto j = hv::Json::parse(fin);
    std::string host = j["host"];
    int port = j["port"];

    ClientManager cm;
    cm.load_clients();
    WebSocketService ws;  // default ping interval is disabled
    ws.onopen = [&cm](WebSocketChannelPtr const& channel, const HttpRequestPtr& req) {
        auto name = req->GetParam("name", "foo");
        cm.add(channel, name);
        std::cout << std::format("client {} connected {}\n", name, req->Path());
    };
    ws.onclose = [&cm](WebSocketChannelPtr const& channel) {
        cm.remove(channel);
        std::cout << "client disconnected\n";
    };

    hv::WebSocketServer server{&ws};
    server.setHost(host.c_str());
    server.setPort(port);
    printf("listening to %s:%d...\n", server.host, server.port);
    server.start();

    lockfree::SPMC<MyData, BUFFER_CAPACITY, MAX_READERS, lockfree::trans::broadcast> queue;
    std::jthread writer{[&queue] {
        size_t index = 0;
        while (true) {
            MyData myData;
            myData.id = index;
            myData.value = index * 0.1;
            snprintf(myData.name, sizeof(myData.name), "Data%zu", index);

            // Attempt to push data into the ring buffer
            while (!queue.push(myData)) {
                std::cout << "Queue is full, cannot push. Retrying...\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            std::cout << std::format("Writer wrote: id={}, value={}, name={}\n", myData.id, myData.value, myData.name);
            ++index;

            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }};

    std::jthread sender{[&cm, &queue] {
        while (true) {
            cm.for_each_client([&queue](WebSocketChannelPtr const& channel, size_t client_id) {
                std::optional<MyData> value;
                auto current_pos = queue.get_read_pos(client_id);
                while (!(value = queue.pop(client_id))) {
                    std::cout << "Queue is empty, consumer " << client_id << " cannot pop.\n";
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }

                auto ptr = reinterpret_cast<const char*>(&value.value());
                std::cout << std::format("send {} begin---------------\n", value.value().id);
                auto ret = channel->send(ptr, sizeof(MyData));
                if (ret < 0) {
                    queue.set_read_pos(client_id, current_pos);
                }
                std::cout << std::format("send {} end {}---------------\n", value.value().id, ret);
            });
        }
    }};
}
