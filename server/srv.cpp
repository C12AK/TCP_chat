#include "reactor.h"

#include <cstdlib>
#include <exception>
#include <format>
#include <iostream>
#include <thread>


// 握手超时：客户端连上后 10 秒内必须完成 ECDH 握手，否则被关闭
#define HANDSHAKE_TIMEOUT_MS 10'000


int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << std::format("Usage: {} <Port>", argv[0]) << std::endl;
        return 1;
    }
    int port = std::atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        std::cerr << "Invalid port" << std::endl;
        return 1;
    }

    std::size_t worker_num = std::thread::hardware_concurrency();
    if (worker_num == 0) worker_num = 4;

    try {
        Reactor reactor(port, worker_num, HANDSHAKE_TIMEOUT_MS);
        reactor.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
