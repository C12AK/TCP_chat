#ifndef CONNECTION_H
#define CONNECTION_H

#include "common.h"
#include "crypto.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>

// 连接生命周期的四个状态
enum class ConnState {
    WAIT_USERNAME,    // 等待握手帧 1：用户名
    WAIT_CLI_PUBKEY,  // 服务器公钥已发出，等待客户端公钥
    READY,            // 握手完成，正常聊天
    CLOSING           // 仅等出站队列刷空，然后关闭（用户名被拒时使用）
};


// 单个连接的全部状态。由 Reactor 线程独占，不加锁
struct Connection {
    int fd;
    int gen;          // 每次新连接自增，供跨线程动作校验 fd 是否被复用
    ConnState state;

    // ---- I/O 缓冲 ----
    std::string inbuf;
    std::size_t in_off = 0;         // inbuf 已消费偏移，避免每帧 erase
    std::deque<std::string> outbuf;
    std::size_t out_head_off = 0;   // outbuf.front() 已发送的字节数
    bool epollout_on = false;

    // ---- 连接信息 ----
    std::string username;
    std::string cli_ip;
    uint16_t cli_port = 0;

    // ---- 加密相关 ----
    Crypto handshake_crypto;                // 仅握手阶段持有
    std::shared_ptr<const vecuc> aeskey;    // 握手完成后可被 worker 线程共享

    // ---- 超时 ----
    int64_t handshake_deadline_ms = 0;
};

#endif // CONNECTION_H
