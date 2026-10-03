#include "db.h"
#include "log.h"
#include "reactor.h"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <format>
#include <iostream>
#include <thread>

// 启动顺序：日志、SQLite、Reactor。TCPCHAT_IDLE_MS 是心跳超时，TCPCHAT_QUEUE_CAP 是每人未完成写库任务的上限
int main(int argc, char* argv[]) {
    if (argc < 2 || argc > 3) {
        std::cerr << std::format("用法: {} <端口> [数据库路径]", argv[0]) << std::endl;
        return 1;
    }
    int port = std::atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        std::cerr << "端口不合法" << std::endl;
        return 1;
    }
    std::string db_path = argc == 3 ? argv[2] : "data/server.db";

    int idle_ms = 45000;
    if (const char* e = std::getenv("TCPCHAT_IDLE_MS")) {
        int v = std::atoi(e);
        if (v >= 1000) idle_ms = v; // 更小的值多半是笔误，保持默认 45 秒
    }
    int queue_cap = 64;
    if (const char* e = std::getenv("TCPCHAT_QUEUE_CAP")) {
        int v = std::atoi(e);
        if (v >= 1) queue_cap = v;
    }

    if (!log_open("logs/server.log", 32u * 1024u * 1024u)) {
        std::cerr << "无法打开日志" << std::endl;
        return 1;
    }

    try {
        Database db(db_path);
        std::size_t workers = std::thread::hardware_concurrency();
        if (workers == 0) workers = 4; // 读不到逻辑处理器个数时不要起 0 个工人
        Reactor reactor(port, db, workers, idle_ms, queue_cap);
        reactor.run();
    } catch (const std::exception& e) {
        log_error(std::string("启动失败: ") + e.what());
        std::cerr << "启动失败: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
