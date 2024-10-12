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

#include "message_generated.h"
#include "queue/spmc.hpp"

void serialize_bar_data(flatbuffers::FlatBufferBuilder& builder, int id) {
    auto bar = Messages::CreateBarDataDirect(builder, id, "apple", 12.3, 1000, 2300.0);
    auto msg = Messages::CreateMessage(builder, Messages::MessageType::BarData, Messages::Payload::BarData, bar.Union());
    builder.Finish(msg);
}

void serialize_tick_data(flatbuffers::FlatBufferBuilder& builder, int id) {
    std::vector<int> vols{1, 2, 3, 45};
    auto tick = Messages::CreateTickDataDirect(builder, id, "msft", 12.3, 12.4, &vols);
    auto msg = Messages::CreateMessage(builder, Messages::MessageType::TickData, Messages::Payload::TickData, tick.Union());
    builder.Finish(msg);
}

void serialize_err_data(flatbuffers::FlatBufferBuilder& builder, const char* text) {
    auto err = Messages::CreateErrDataDirect(builder, text);
    auto msg = Messages::CreateMessage(builder, Messages::MessageType::ErrData, Messages::Payload::ErrData, err.Union());
    builder.Finish(msg);
}

#define BUFFER_CAPACITY 128
#define MAX_READERS 16

int main(int argc, char** argv) {
    std::array<std::atomic<WebSocketChannelPtr>, MAX_READERS> channels{};  // all nullptr
    WebSocketService ws;                                                   // default ping interval is disabled
    ws.onopen = [&channels](WebSocketChannelPtr const& channel, const HttpRequestPtr& req) {
        auto id_str = req->GetParam("id", "0");
        size_t id = 0;  // default value if parsing fails
        auto [ptr, ec] = std::from_chars(id_str.data(), id_str.data() + id_str.size(), id);

        flatbuffers::FlatBufferBuilder builder;

        if (ec != std::errc() || id >= MAX_READERS) {
            serialize_err_data(builder, std::format("err,id>={}", MAX_READERS).c_str());
            channel->send(reinterpret_cast<const char*>(builder.GetBufferPointer()), builder.GetSize());
            channel->close();  // invalid id, close connection
            return;
        }

        WebSocketChannelPtr expected{nullptr};
        if (channels[id].compare_exchange_strong(expected, channel, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            // compare contained with expected, if true, set contained to channel
            std::cout << std::format("client {} connected {}\n", id, req->Path());
            // set context for channel
            // contained ptr in channels[id] is atomic, but the data it pointed to can be modified by channel
            channel->setContextPtr(std::make_shared<size_t>(id));
        } else {
            // compare contained with expected, if false, set expected to contained
            serialize_err_data(builder, std::format("err,id={} in use", MAX_READERS).c_str());
            channel->send(reinterpret_cast<const char*>(builder.GetBufferPointer()), builder.GetSize());
            channel->close();
            return;
        }
    };
    ws.onclose = [&channels](WebSocketChannelPtr const& channel) {
        if (auto id_ptr = channel->getContextPtr<size_t>(); id_ptr) {
            channels[*id_ptr].store(nullptr, std::memory_order_release);
            std::cout << std::format("client {} disconnected\n", *id_ptr);
        } else {
            std::cout << "client without id disconnected\n";
        }
    };

    hv::WebSocketServer server{&ws};
    server.setHost("localhost");
    server.setPort(8888);
    std::cout << std::format("listening to {}:{}...\n", server.host, server.port);
    server.start();

    lockfree::SPMC<int, BUFFER_CAPACITY, MAX_READERS, lockfree::trans::broadcast> bar_queue;
    lockfree::SPMC<int, BUFFER_CAPACITY, MAX_READERS, lockfree::trans::broadcast> tick_queue;
    std::jthread bar_writer{[&bar_queue] {
        size_t index = 0;
        while (true) {
            // Attempt to push data into the ring buffer
            while (!bar_queue.push_overwrite(index)) {
                std::cout << "Queue is full, cannot push. Retrying...\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            std::cout << std::format("Writer wrote: bar id={}\n", index);
            ++index;

            std::this_thread::sleep_for(std::chrono::milliseconds(3000));
        }
    }};
    std::jthread tick_writer{[&tick_queue] {
        size_t index = 0;
        while (true) {
            // Attempt to push data into the ring buffer
            while (!tick_queue.push_overwrite(index)) {
                std::cout << "Queue is full, cannot push. Retrying...\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            std::cout << std::format("Writer wrote: tick id={}\n", index);
            ++index;

            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
    }};

    std::jthread bar_sender{[&channels, &bar_queue] {
        while (true) {
            bool all_has_data = false;
            flatbuffers::FlatBufferBuilder builder;
            for (size_t i = 0; i < MAX_READERS; ++i) {
                auto channel = channels[i].load(std::memory_order_acquire);
                if (channel) {
                    if (auto data = bar_queue.pop_overwrite(i); data.has_value()) {
                        serialize_bar_data(builder, data.value());
                        auto ret = channel->send(reinterpret_cast<const char*>(builder.GetBufferPointer()), builder.GetSize());
                        if (ret < 0) {
                            // Send failed, adjust the read position
                            bar_queue.fetch_sub_read_pos(i, 1);
                        }
                        std::cout << std::format("send bar {} to {}, ret={}------------\n", data.value(), i, ret);
                        all_has_data = true;
                        builder.Clear();
                    }
                }
            }

            // all not has_data
            if (!all_has_data) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));  // should smaller than writer interval
            }
        }
    }};

    std::jthread tick_sender{[&channels, &tick_queue] {
        while (true) {
            bool all_has_data = false;
            flatbuffers::FlatBufferBuilder builder;
            for (size_t i = 0; i < MAX_READERS; ++i) {
                auto channel = channels[i].load(std::memory_order_acquire);
                if (channel) {
                    if (auto data = tick_queue.pop_overwrite(i); data.has_value()) {
                        serialize_tick_data(builder, data.value());
                        auto ret = channel->send(reinterpret_cast<const char*>(builder.GetBufferPointer()), builder.GetSize());
                        if (ret < 0) {
                            // Send failed, adjust the read position
                            tick_queue.fetch_sub_read_pos(i, 1);
                        }
                        std::cout << std::format("send tick {} to {}, ret={}------------\n", data.value(), i, ret);
                        all_has_data = true;
                        builder.Clear();
                    }
                }
            }

            // all not has_data
            if (!all_has_data) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));  // should smaller than writer interval
            }
        }
    }};
}