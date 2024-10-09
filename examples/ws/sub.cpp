#include <hv/WebSocketClient.h>
#include <hv/http_content.h>

#include <cstdio>
#include <fstream>

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
        std::cout << "recv: " << msg << '\n';
    };
    ws.onclose = []() {
        printf("onclose\n");
    };

    ws.open(addr.c_str());

    getchar();
}