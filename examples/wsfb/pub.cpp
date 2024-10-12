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
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "message_generated.h"
#include "queue/spmc.hpp"

#define BUFFER_CAPACITY 128
#define MAX_READERS 16

// Type alias for the lock-free queue holding serialized data
template <typename T>
using SPMCQueue = lockfree::SPMC<T, BUFFER_CAPACITY, MAX_READERS, lockfree::trans::broadcast>;

// Serialization functions that return serialized data
std::vector<uint8_t> serialize_bar_data(int id) {
    flatbuffers::FlatBufferBuilder builder;
    auto bar = Messages::CreateBarDataDirect(builder, id, "apple", 12.3, 1000, 2300.0);
    auto msg = Messages::CreateMessage(builder, Messages::MessageType::BarData, Messages::Payload::BarData, bar.Union());
    builder.Finish(msg);
    auto size = builder.GetSize();
    auto buffer_ptr = builder.GetBufferPointer();
    // Copy the data into a vector
    return std::vector<uint8_t>(buffer_ptr, buffer_ptr + size);
}

std::vector<uint8_t> serialize_tick_data(int id) {
    flatbuffers::FlatBufferBuilder builder;
    std::vector<int> vols{1, 2, 3, 45};
    auto tick = Messages::CreateTickDataDirect(builder, id, "msft", 12.3, 12.4, &vols);
    auto msg = Messages::CreateMessage(builder, Messages::MessageType::TickData, Messages::Payload::TickData, tick.Union());
    builder.Finish(msg);
    auto size = builder.GetSize();
    auto buffer_ptr = builder.GetBufferPointer();
    // Copy the data into a vector
    return std::vector<uint8_t>(buffer_ptr, buffer_ptr + size);
}

void serialize_err_data(flatbuffers::FlatBufferBuilder& builder, const char* text) {
    auto err = Messages::CreateErrDataDirect(builder, text);
    auto msg = Messages::CreateMessage(builder, Messages::MessageType::ErrData, Messages::Payload::ErrData, err.Union());
    builder.Finish(msg);
}

// Generalized writer thread function
template <typename QueueType, typename SerializeFunc>
void writer_thread(QueueType& queue, SerializeFunc serialize_func, const std::string& data_type, std::chrono::milliseconds interval) {
    size_t index = 0;
    while (true) {
        // Attempt to push data into the ring buffer
        while (!queue.push_overwrite(serialize_func(index))) {
            std::cout << "Queue is full for " << data_type << ", cannot push. Retrying...\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::cout << std::format("Writer wrote: {} id={}\n", data_type, index);
        ++index;
        std::this_thread::sleep_for(interval);
    }
}

// Generalized sender thread function
template <typename QueueType>
void sender_thread(std::array<std::atomic<WebSocketChannelPtr>, MAX_READERS>& channels,
                   QueueType& queue,
                   const std::string& data_type) {
    while (true) {
        bool any_data_sent = false;
        for (size_t i = 0; i < MAX_READERS; ++i) {
            auto channel = channels[i].load(std::memory_order_acquire);
            if (channel) {
                if (auto data = queue.pop_overwrite(i); data.has_value()) {
                    auto& serialized_data = data.value();
                    auto ret = channel->send(reinterpret_cast<const char*>(serialized_data.data()), serialized_data.size());
                    if (ret < 0) {
                        // Send failed, adjust the read position
                        queue.fetch_sub_read_pos(i, 1);
                    }
                    std::cout << std::format("Sent {} data to client {}, ret={}\n", data_type, i, ret);
                    any_data_sent = true;
                }
            }
        }
        if (!any_data_sent) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));  // Should be smaller than writer interval
        }
    }
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

    // Queues for serialized data
    SPMCQueue<std::vector<uint8_t>> bar_queue;
    SPMCQueue<std::vector<uint8_t>> tick_queue;

    // Writer threads with serialization functions
    std::jthread bar_writer(writer_thread<decltype(bar_queue), decltype(serialize_bar_data)>,
                            std::ref(bar_queue), serialize_bar_data, "bar", std::chrono::milliseconds(3000));

    std::jthread tick_writer(writer_thread<decltype(tick_queue), decltype(serialize_tick_data)>,
                             std::ref(tick_queue), serialize_tick_data, "tick", std::chrono::milliseconds(1000));

    // Sender threads
    std::jthread bar_sender(sender_thread<decltype(bar_queue)>,
                            std::ref(channels), std::ref(bar_queue), "bar");

    std::jthread tick_sender(sender_thread<decltype(tick_queue)>,
                             std::ref(channels), std::ref(tick_queue), "tick");

    // Keep the main thread alive
    std::this_thread::sleep_for(std::chrono::hours(24));

    return 0;
}
