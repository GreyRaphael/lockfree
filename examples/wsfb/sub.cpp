#include <flatbuffers/flatbuffers.h>
#include <hv/WebSocketClient.h>
#include <hv/http_content.h>

#include <cstdio>
#include <format>

#include "message_generated.h"

void deserialize_messages(const char* buffer, size_t size) {
    auto msg_ptr = Messages::GetMessage(buffer);

    if (msg_ptr->type() == Messages::MessageType::BarData) {
        auto bar = msg_ptr->payload_as_BarData();
        std::cout << std::format("recv BarData, id={}, symbol={}, price={}, volume={}, amount={}\n", bar->id(), bar->symbol()->str(), bar->price(), bar->volume(), bar->amount());
    } else if (msg_ptr->type() == Messages::MessageType::TickData) {
        auto tick = msg_ptr->payload_as_TickData();
        std::cout << std::format("recv TickData, id={}, symbol={}, open={}, high={}, volumes=[", tick->id(), tick->symbol()->str(), tick->open(), tick->high());

        for (auto volume : *tick->volumes()) {
            std::cout << std::format("{} ", volume);
        }
        std::cout << "]\n";
    } else if (msg_ptr->type() == Messages::MessageType::ErrData) {
        auto err = msg_ptr->payload_as_ErrData();
        std::cout << std::format("recv ErrData, text={}\n", err->text()->str());
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