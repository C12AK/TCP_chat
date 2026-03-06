#include "crypto.h"

#include <iostream>
#include <cstring>
#include <format>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <stdexcept>
#include <chrono>
#include <vector>
#include <numeric>

#define BUFSZ 1024

Crypto crypto{};
int LOOPS;          // 每个子进程发送多少次消息。由于 [1] 和 [2] ，值应当适中
std::string msg;    // 发送的消息


// ==================== 工具函数声明 ====================
inline void send_msg(int sock, const std::string& to, const std::string& msg);
void Send(int sock, const char* sp, int len);
inline void process_msg(const char* buf, int len, std::string& from, std::string& msg);


// ==================== 子进程 ====================
void task(const std::string& ip, const std::string& port, const std::string& username) {
    int sock = socket(PF_INET, SOCK_STREAM, 0);
    if (sock < 0) _exit(1);     // exit() 会默认刷缓冲区

    sockaddr_in srv_addr{};
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_addr.s_addr = inet_addr(ip.c_str());
    srv_addr.sin_port = htons(std::stoi(port));

    if (connect(sock, (sockaddr *)&srv_addr, sizeof(srv_addr)) < 0) {
        close(sock);
        _exit(1);
    }

    char buf[BUFSZ];

    // ------------------------------
    // 由于连接初始化这部分在服务器端是阻塞调用，所以 LOOPS 不能太大，否则会掩盖这个缺点    [2]
    // ------------------------------

    // 初始化连接阶段（不计入 QPS 统计）
    send(sock, username.c_str(), username.length(), MSG_NOSIGNAL);
    int len = recv(sock, buf, BUFSZ - 1, 0);
    if (len <= 0) {
        close(sock);
        _exit(1);
    }
    
    vecuc ecdh_pubkey(buf, buf + len);

    try {
        crypto.generate_ecdh_keypr();
        vecuc client_ecdh_pubkey = crypto.get_ecdh_pubkey();
        crypto.set_peer_ecdh_pubkey(ecdh_pubkey);
        
        static const vecuc fixed_salt = {0x11, 0x45, 0x14, 0x19, 0x19, 0x81, 0x0f, 0x91, 
                                        0x0d, 0x00, 0x07, 0x21, 0xc1, 0x2a, 0xc1, 0x01};
        crypto.derive_shared_secret(&fixed_salt);
        
        send(sock, client_ecdh_pubkey.data(), client_ecdh_pubkey.size(), MSG_NOSIGNAL);
        len = recv(sock, buf, BUFSZ - 1, 0);
        if (len <= 0) {
            close(sock);
            _exit(1);
        }
    } catch (...) {
        close(sock);
        _exit(1);
    }

    // 压测阶段
    auto start_test = std::chrono::high_resolution_clock::now();
    int unsuccessful_cnt{};

    for (int i = 0; i < LOOPS; ++i) {
        send_msg(sock, username, msg);
        
        int totlen{};
        while((size_t)totlen < msg.length()) {
            int len = recv(sock, buf, BUFSZ - 1, 0);
            if (len < 0) {
                ++unsuccessful_cnt;
                break;
            }
            totlen += len;
        }
    }

    auto end_test = std::chrono::high_resolution_clock::now();
    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end_test - start_test).count();

    if(unsuccessful_cnt > LOOPS / 100) std::cerr << "1 PROC ERROR\n";   // 必须保证可用性

    close(sock);
    _exit(0);
}

// ==================== 主函数 ====================
int main(int argc, char* argv[]) {
    std::string numstr = "10000", loopstr = "100", lenstr = "2000", ipstr = "127.0.0.1", portstr = "8080";

    if (argc >= 2) numstr = argv[1];
    if (argc >= 3) loopstr = argv[2];
    if (argc >= 4) lenstr = argv[3];
    if (argc >= 5) ipstr = argv[4];
    if (argc >= 6) portstr = argv[5];

    int CNUM = std::stoi(numstr);
    LOOPS = std::stoi(loopstr);
    int LEN = std::stoi(lenstr);
    std::vector<pid_t> pids(CNUM);

    // 构造发的消息
    for (int i = 0; i < LEN; ++i) msg += 'a';

    // 计时区间包含了创建子进程的耗时，所以 LOOPS 不能太小                  [1]
    auto global_start = std::chrono::high_resolution_clock::now();  // 计时开始

    for (int i = 0; i < CNUM; ++i) {
        pid_t pid = fork();
        if (pid == 0) {
            // 子进程
            task(ipstr, portstr, "user_" + std::to_string(i));      // 子进程压测
            exit(0);
        } else if (pid > 0) {
            pids[i] = pid;
        } else {
            perror("fork");
            return 1;
        }
    }

    for (int i = 0; i < CNUM; ++i) wait(nullptr);                   // 等待所有子进程结束
    auto global_end = std::chrono::high_resolution_clock::now();    // 计时结束

    auto total_duration_ms = std::chrono::duration<double, std::milli>(global_end - global_start).count();
    long total_messages = static_cast<long>(CNUM) * LOOPS;
    
    // QPS = 总消息数 / 总耗时（秒）
    double qps = (total_duration_ms > 0) ? (total_messages / (total_duration_ms / 1000.0)) : 0;
    double avg_latency_ms = (total_messages > 0) ? (total_duration_ms / total_messages) : 0;

    std::cout << "----------------------------------------\n";
    std::cout << "Total Clients   : " << CNUM << "\n";
    std::cout << "Loops per Client: " << LOOPS << "\n";
    std::cout << "Message Length  : " << LEN << "\n";
    std::cout << "Total Messages  : " << total_messages << "\n";
    std::cout << "Total Time      : " << total_duration_ms << " ms\n";
    std::cout << "Overall QPS     : " << std::format("{:.2f}", qps) << " msgs/sec\n";
    std::cout << "Avg Latency     : " << std::format("{:.4f}", avg_latency_ms) << " ms/msg\n";
    std::cout << "----------------------------------------\n";
    
    return 0;
}

// ==================== 工具函数实现 ====================
inline void send_msg(int sock, const std::string& to, const std::string& msg) {
    std::string c_to, c_msg;
    try {
        c_to = crypto.aes_encrypt(to), c_msg = crypto.aes_encrypt(msg);
    } catch (const std::exception& e) {
        std::cerr << "AES encrypt: " << e.what() << std::endl;
        return;
    }

    uint16_t n_tolen = htons(static_cast<uint16_t>(c_to.length()));
    uint32_t n_msglen = htonl(static_cast<uint32_t>(c_msg.length()));
    std::string pck;
    pck.reserve(sizeof(n_tolen) + sizeof(n_msglen) + c_to.length() + c_msg.length());
    pck.append(reinterpret_cast<const char*>(&n_tolen), sizeof(n_tolen));
    pck.append(reinterpret_cast<const char*>(&n_msglen), sizeof(n_msglen));
    pck += c_to, pck += c_msg;

    try {
        Send(sock, pck.c_str(), pck.length());
    } catch (...) {}
}