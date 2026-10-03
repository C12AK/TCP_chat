#ifndef CONNECTION_H
#define CONNECTION_H

#include <cstdint>
#include <deque>
#include <string>

enum class ConnState {
    Fresh,    // 已连接，尚未登录
    Ready,    // 已登录
    Closing   // 等发送队列排空后关闭
};

// 单个连接的全部状态。由 Reactor 线程独占，不加锁
struct Connection {
    int fd = -1;
    int gen = 0;          // 每次接入自增。线程池回投时用它确认 fd 没有被后来的连接复用
    ConnState state = ConnState::Fresh;

    std::string inbuf;
    std::size_t in_off = 0;         // inbuf 里已经切成帧的偏移，避免每帧都 memmove
    std::deque<std::string> outbuf;
    std::size_t out_head_off = 0;   // outbuf.front() 已写出的字节数
    bool epollout_on = false;

    uint64_t user_id = 0;           // 登录成功后才非 0
    std::string username;
    std::string cli_ip;
    uint16_t cli_port = 0;
    int64_t last_rx_ms = 0;         // 最近一次收到字节的时间，供心跳超时使用
};

#endif
