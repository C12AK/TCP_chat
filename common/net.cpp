#include "net.h"
#include "protocol.h"

#include <cerrno>
#include <cstring>
#include <cstdint>
#include <stdexcept>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>


#define BUFSZ 4096
#define MAX_RETRIES 100     // 最大重试次数


// ==================== 阻塞式完整发送 ====================
void blocking_send_all(int sock, const char* data, std::size_t len) {
    std::size_t sent = 0;
    int retries = 0;
    while (sent < len) {
        ssize_t n = send(sock, data + sent, len - sent, MSG_NOSIGNAL);  // 禁止 SIGPIPE ，改为 errno = EPIPE
        if (n <= 0) {
            if ((errno == EAGAIN || errno == EWOULDBLOCK) && retries < MAX_RETRIES) {
                ++retries;
                usleep(1000);   // 发送缓冲区满则稍后重试
                continue;
            } else if (errno == EBADF) {
                return;         // 对端已关闭，直接结束
            } else {
                throw std::runtime_error("send error");
            }
        }
        sent += n;
        retries = 0;
    }
}


// ==================== 阻塞式接收一个 KA 帧 ====================
int blocking_recv_ka_frame(int sock, std::vector<unsigned char>& out) {
    std::string buf;
    char tmp[BUFSZ];

    while (true) {
        // 尝试解析现有缓冲区，够了就返回
        try {
            auto payload = parse_ka_frame(buf);
            if (payload) {
                out.assign(payload->begin(), payload->end());
                return static_cast<int>(out.size());
            }
        } catch (const std::exception&) {
            return -1;
        }

        ssize_t n = recv(sock, tmp, sizeof(tmp), 0);
        if (n == 0) return 0;
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf.append(tmp, n);
    }
}


// ==================== fd 设为非阻塞 ====================
void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return;
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}


// ==================== 关闭 Nagle ====================
void set_tcp_nodelay(int fd) {
    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}


// ==================== 创建监听 socket ====================
int make_listen_socket(int port, int backlog) {
    int sock = socket(PF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    if (listen(sock, backlog) < 0) {
        close(sock);
        return -1;
    }
    return sock;
}
