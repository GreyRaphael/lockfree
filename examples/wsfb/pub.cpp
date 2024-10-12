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

    lockfree::SPMC<int, BUFFER_CAPACITY, MAX_READERS, lockfree::trans::broadcast> queue;
    std::jthread writer{[&queue] {
        size_t index = 0;
        while (true) {
            // Attempt to push data into the ring buffer
            // while (!queue.push(myData)) {
            while (!queue.push_overwrite(index)) {
                std::cout << "Queue is full, cannot push. Retrying...\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            std::cout << std::format("Writer wrote: id={}\n", index);
            ++index;

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }};

    std::jthread sender{[&channels, &queue] {
        while (true) {
            bool all_has_data = false;
            flatbuffers::FlatBufferBuilder bar_builder;
            flatbuffers::FlatBufferBuilder tick_builder;
            for (size_t i = 0; i < MAX_READERS; ++i) {
                auto channel = channels[i].load(std::memory_order_acquire);
                if (channel) {
                    // if (auto data = queue.pop(i); data.has_value()) {
                    if (auto data = queue.pop_overwrite(i); data.has_value()) {
                        serialize_bar_data(bar_builder, data.value());
                        serialize_tick_data(tick_builder, data.value());
                        auto ret1 = channel->send(reinterpret_cast<const char*>(bar_builder.GetBufferPointer()), bar_builder.GetSize());
                        auto ret2 = channel->send(reinterpret_cast<const char*>(tick_builder.GetBufferPointer()), tick_builder.GetSize());
                        if (ret1 < 0 || ret2 < 0) {
                            // Send failed, adjust the read position
                            queue.fetch_sub_read_pos(i, 1);
                        }
                        std::cout << std::format("send {} to {}, ret={}------------\n", data.value(), i, ret1 + ret2);
                        all_has_data = true;
                        bar_builder.Clear();
                        tick_builder.Clear();
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