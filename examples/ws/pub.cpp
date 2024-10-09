#include <hv/Event.h>
#include <hv/EventLoop.h>
#include <hv/WebSocketChannel.h>
#include <hv/WebSocketServer.h>
#include <hv/http_content.h>

#include <chrono>
#include <format>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_set>

// Thread-safe container for clients
class ClientManager {
    mutable std::shared_mutex mutex_;
    std::unordered_set<WebSocketChannelPtr> clients_;

   public:
    void add(WebSocketChannelPtr const& channel) {
        std::unique_lock lock(mutex_);
        clients_.emplace(channel);
    }

    void remove(WebSocketChannelPtr const& channel) {
        std::unique_lock lock(mutex_);
        clients_.erase(channel);
    }

    // Allows safe iteration over clients without copying
    void for_each_client(auto&& func) const {
        std::shared_lock lock(mutex_);
        for (const auto& client : clients_) {
            func(client);
        }
    }
};

int main(int argc, char** argv) {
    std::ifstream fin{"server.json"};
    auto j = hv::Json::parse(fin);
    std::string host = j["host"];
    int port = j["port"];

    ClientManager cm;
    WebSocketService ws;  // default ping interval is disabled
    ws.onopen = [&cm](WebSocketChannelPtr const& channel, const HttpRequestPtr& req) {
        cm.add(channel);
        printf("onopen: GET %s\n", req->Path().c_str());
    };
    ws.onclose = [&cm](WebSocketChannelPtr const& channel) {
        cm.remove(channel);
        printf("onclose\n");
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
