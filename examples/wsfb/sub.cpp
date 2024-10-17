#include <flatbuffers/flatbuffers.h>
#include <hv/WebSocketClient.h>
#include <hv/http_content.h>

#include <cstdio>
#include <format>

#include "message_generated.h"

void deserialize_messages(const char* buffer, size_t size) {
    auto msg_ptr = Messages::GetMessage(buffer);

    switch (msg_ptr->payload_type()) {
        case Messages::Payload::BarData: {
            auto bar = msg_ptr->payload_as_BarData();
            std::cout << std::format("recv BarData, id={}, symbol={}, price={:.2f}, volume={}, amount={:.2f}\n", bar->id(), bar->symbol()->str(), bar->price(), bar->volume(), bar->amount());
            break;
        }
        case Messages::Payload::TickData: {
            auto tick = msg_ptr->payload_as_TickData();
            std::cout << std::format("recv TickData, id={}, symbol={}, open={:.2f}, high={:.2f}, volumes=[", tick->id(), tick->symbol()->str(), tick->open(), tick->high());

            for (auto volume : *tick->volumes()) {
                std::cout << std::format("{} ", volume);
            }
            std::cout << "]\n";
            break;
        }
        case Messages::Payload::ErrData: {
            auto err = msg_ptr->payload_as_ErrData();
            std::cout << std::format("recv ErrData, text={}\n", err->text()->str());
            break;
        }
        default:
            std::cout << "unknown payload type\n";
            break;
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: %s id\n", argv[0]);
        return -1;
    }
    std::string addr = std::format("localhost:8888/v1?id={}", argv[1]);

    hv::WebSocketClient ws;
    ws.setPingInterval(0);

    ws.onopen = []() {
        printf("onopen\n");
    };
    ws.onmessage = [](const std::string& msg) {
        deserialize_messages(msg.data(), msg.size());
    };
    ws.onclose = [] {
        printf("onclose\n");
    };

    ws.open(addr.c_str());

    getchar();
}