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
        for (const auto& [client, _] : channels_) {
            func(client);
        }
    }

    // find client_id in the clients_map_
    size_t retrive_id(std::string const& name) {
        if (clients_map_.contains(name)) {
            return clients_map_[name];
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

    std::jthread sender{[&cm] {
        int i = 0;
        while (true) {
            cm.for_each_client([i](WebSocketChannelPtr const& channel) {
                auto msg = std::format("hello-{}", i);
                channel->send(msg);
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            ++i;
        }
    }};
}
