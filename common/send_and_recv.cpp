#include <iostream>
#include <sys/socket.h>
#include <cerrno>
#include <unistd.h>
#include <stdexcept>
#include <vector>
#include <string>
#include <cstring>
#include <cstdint>
#include <arpa/inet.h>

#define MAX_RETRIES 100     // 最大重试次数
#define BUFSZ 1024


void Send(int sock, const char* sp, int len) {
    int sent = 0, retries = 0;
    while (sent < len) {
        int n = send(sock, sp + sent, len - sent, MSG_NOSIGNAL);  // 禁止 SIGPIPE ，而是返回 -1 且 errno = EPIPE
        if (n <= 0) {
            if ((errno == EAGAIN || errno == EWOULDBLOCK) && retries < MAX_RETRIES) {
                retries++;
                usleep(1000);   // 发送缓冲区满则稍后重试。更好的实现是检测到 EPOLLOUT 时继续发送，但需要更复杂的整体架构
                continue;
            } else if (errno == EBADF) {
                return;         // 对端已关闭，直接结束发送即可
            } else {
                throw std::runtime_error("send of Send error");
                return;
            }
        }
        sent += n;
        retries = 0;    // 发送成功一次后，重置重试次数
    }

    if (sent < len) {
        throw std::runtime_error("in Send partially sent");
        return;
    }
}


// 内容前面加上 4 字节长度
void send_for_ka(int sock, const unsigned char* vp, int len) {
    std::string s;
    s.reserve(len + 4);
    uint32_t n_len = htonl(static_cast<uint32_t>(len));
    s.append(reinterpret_cast<const char*>(&n_len), sizeof(n_len));
    s.append(std::string(reinterpret_cast<const char*>(vp), len));
    Send(sock, s.c_str(), s.length());
}


// 内容放进 vp, 长度放进 len, 调用方实现错误处理
void recv_for_ka(int sock, std::vector<unsigned char>& vp, int& len) {
    int tot{}, expected;
    char buf[BUFSZ];
    std::string s{};

    while (tot < 4) {
        int rlen = recv(sock, buf, BUFSZ - 1, 0);
        if (rlen <= 0) {
            len = rlen;
            return;
        }
        tot += rlen;
        s += std::string(buf, rlen);
    }

    uint32_t n_len;
    memcpy(&n_len, s.c_str(), sizeof(n_len));
    expected = ntohl(n_len);
    tot -= 4;

    while (tot < expected) {
        int rlen = recv(sock, buf, BUFSZ - 1, 0);
        if (rlen <= 0) {
            len = rlen;
            return;
        }
        tot += rlen;
        s += std::string(buf, rlen);
    }

    vp = std::vector<unsigned char>(s.begin() + 4, s.begin() + 4 + expected);
    len = expected;
}