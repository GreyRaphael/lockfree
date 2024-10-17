#include <flatbuffers/flatbuffers.h>
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
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "message_generated.h"
#include "queue/spmc.hpp"
#include "quote.h"

#define BUFFER_CAPACITY 128
#define MAX_READERS 16

// Type alias for the lock-free queue
template <typename T>
using SPMCQueue = lockfree::SPMC<T, BUFFER_CAPACITY, MAX_READERS, lockfree::trans::broadcast>;

// Generalized sender thread function
template <typename QueueType, typename SerializeFunc>
void sender_thread(std::array<std::atomic<WebSocketChannelPtr>, MAX_READERS>& channels,
                   QueueType& queue,
                   SerializeFunc serialize_func,
                   std::string_view name) {
    flatbuffers::FlatBufferBuilder builder;
    while (true) {
        bool any_data_sent = false;
        for (size_t i = 0; i < MAX_READERS; ++i) {
            auto channel = channels[i].load(std::memory_order_acquire);
            if (channel) {
                if (auto data = queue.pop_overwrite(i); data.has_value()) {
                    serialize_func(builder, data.value());
                    auto ret = channel->send(reinterpret_cast<const char*>(builder.GetBufferPointer()), builder.GetSize());
                    if (ret < 0) {
                        // Send failed, adjust the read position
                        queue.fetch_sub_read_pos(i, 1);
                    }
                    std::cout << std::format("{} send {} bytes to client-{}, ret={}\n", name, sizeof(builder.GetSize()), i, ret);
                    any_data_sent = true;
                }
            }
        }
        if (!any_data_sent) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));  // Should be smaller than writer interval
        }
    }
}

// Serialization functions
void serialize_bar_data(flatbuffers::FlatBufferBuilder& builder, BarData const& bar) {
    builder.Clear();
    auto bar_offset = Messages::CreateBarDataDirect(builder, bar.id, bar.symbol, bar.price, bar.volume, bar.amount);
    auto msg = Messages::CreateMessage(builder, Messages::Payload::BarData, bar_offset.Union());
    builder.Finish(msg);
}

void serialize_tick_data(flatbuffers::FlatBufferBuilder& builder, TickData const& tick) {
    builder.Clear();
    std::vector<int> vols{std::begin(tick.volumes), std::end(tick.volumes)};
    auto tick_offset = Messages::CreateTickDataDirect(builder, tick.id, tick.symbol, tick.open, tick.high, &vols);
    auto msg = Messages::CreateMessage(builder, Messages::Payload::TickData, tick_offset.Union());
    builder.Finish(msg);
}

void serialize_err_data(flatbuffers::FlatBufferBuilder& builder, const char* text) {
    auto err = Messages::CreateErrDataDirect(builder, text);
    auto msg = Messages::CreateMessage(builder, Messages::Payload::ErrData, err.Union());
    builder.Finish(msg);
}

int main(int argc, char** argv) {
    std::array<std::atomic<WebSocketChannelPtr>, MAX_READERS> channels{};  // all nullptr

    // WebSocket service setup
    WebSocketService ws;
    ws.onopen = [&channels](WebSocketChannelPtr const& channel, const HttpRequestPtr& req) {
        auto id_str = req->GetParam("id", "0");
        size_t id = 0;  // default value if parsing fails
        auto [ptr, ec] = std::from_chars(id_str.data(), id_str.data() + id_str.size(), id);

        flatbuffers::FlatBufferBuilder builder;

        if (ec != std::errc() || id >= MAX_READERS) {
            serialize_err_data(builder, std::format("Error: Invalid ID (>= {})", MAX_READERS).c_str());
            channel->send(reinterpret_cast<const char*>(builder.GetBufferPointer()), builder.GetSize());
            channel->close();  // Invalid ID, close connection
            return;
        }

        WebSocketChannelPtr expected{nullptr};
        if (channels[id].compare_exchange_strong(expected, channel, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            std::cout << std::format("Client {} connected {}\n", id, req->Path());
            channel->setContextPtr(std::make_shared<size_t>(id));
        } else {
            serialize_err_data(builder, std::format("Error: ID {} in use", id).c_str());
            channel->send(reinterpret_cast<const char*>(builder.GetBufferPointer()), builder.GetSize());
            channel->close();
            return;
        }
    };

    ws.onclose = [&channels](WebSocketChannelPtr const& channel) {
        if (auto id_ptr = channel->getContextPtr<size_t>(); id_ptr) {
            channels[*id_ptr].store(nullptr, std::memory_order_release);
            std::cout << std::format("Client {} disconnected\n", *id_ptr);
        } else {
            std::cout << "Client without ID disconnected\n";
        }
    };

    // WebSocket server setup
    hv::WebSocketServer server{&ws};
    server.setHost("localhost");
    server.setPort(8888);
    std::cout << std::format("Listening on {}:{}...\n", server.host, server.port);
    server.start();

    // Queues
    SPMCQueue<BarData> bar_queue;
    SPMCQueue<TickData> tick_queue;

    // Writer threads

    std::jthread bar_writer{[&bar_queue](std::string_view name, int interval) {
                                int index = 0;
                                while (true) {
                                    BarData bar{index, "MSFT", 1.1 * index, 100 * index, 10.1 * index};
                                    while (!bar_queue.push_overwrite(bar)) {
                                        std::cout << "queue full, sleeping...\n";
                                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                                    }
                                    std::cout << std::format("{} writer pushed id={}, symbol={}, vol={}\n", name, bar.id, bar.symbol, bar.volume);
                                    ++index;
                                    std::this_thread::sleep_for(std::chrono::milliseconds(interval));
                                }
                            },
                            "bar", 3000};

    std::jthread tick_writer{[&tick_queue](std::string_view name, int interval) {
                                 int index = 0;
                                 while (true) {
                                     TickData tick{index, "APPL", 1.1 * index, 1.2 * index, {index, index * 2, index * 3}};
                                     while (!tick_queue.push_overwrite(tick)) {
                                         std::cout << "queue full, sleeping...\n";
                                         std::this_thread::sleep_for(std::chrono::milliseconds(100));
                                     }
                                     std::cout << std::format("{} writer pushed id={}, symbol={}, vol1={}\n", name, tick.id, tick.symbol, tick.volumes[0]);
                                     ++index;
                                     std::this_thread::sleep_for(std::chrono::milliseconds(interval));
                                 }
                             },
                             "tick", 1000};

    // Serialization lambdas
    // Sender threads
    std::jthread bar_sender(sender_thread<decltype(bar_queue), decltype(serialize_bar_data)>,
                            std::ref(channels), std::ref(bar_queue), serialize_bar_data, "bar");

    std::jthread tick_sender(sender_thread<decltype(tick_queue), decltype(serialize_tick_data)>,
                             std::ref(channels), std::ref(tick_queue), serialize_tick_data, "tick");

    // Keep the main thread alive
    std::this_thread::sleep_for(std::chrono::hours(24));

    return 0;
}
