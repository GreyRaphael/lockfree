#include <hv/WebSocketClient.h>
#include <hv/http_content.h>

#include <cstdio>
#include <fstream>

struct MyData {
    int id;
    double value;
    char name[16];
};

int main(int argc, char** argv) {
    std::ifstream fin{"client.json"};
    auto j = hv::Json::parse(fin);
    std::string addr = j["addr"];

    hv::WebSocketClient ws;
    ws.setPingInterval(0);

    ws.onopen = []() {
        printf("onopen\n");
    };
    ws.onmessage = [](const std::string& msg) {
        auto ptr = reinterpret_cast<const MyData*>(msg.data());
        printf("recv: %d %f %s\n", ptr->id, ptr->value, ptr->name);
    };
    ws.onclose = [] {
        printf("onclose\n");
    };

    ws.open(addr.c_str());

    getchar();
    ws.close();
}