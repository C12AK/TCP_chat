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
#define MAX_RETRIES 100     // 发送缓冲区满时的最多重试次数


// ==================== 阻塞式完整发送 ====================
// 把 len 个字节全部写出去才返回。对端已关则直接结束；其它发送错误抛异常。
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


// ==================== 阻塞式接收一帧 ====================
// 字节不够就继续 recv。返回 1 成功，0 对端关闭，-1 错误或帧非法。buf 里留下还没切走的尾巴。
int blocking_recv_frame(int sock, std::string& buf, Frame& out) {
    char tmp[BUFSZ];
    std::size_t off = 0;
    while (true) {
        try {
            auto frame = parse_frame(buf, off);
            if (frame) {
                if (off > 0) buf.erase(0, off);
                out = std::move(*frame);
                return 1;
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
// 之后 read/write 在没有数据时返回 EAGAIN，不会把调用线程睡死。
void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return;
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}


// ==================== 关闭 Nagle ====================
// 小帧立刻发出去。开着 Nagle 时，短消息会被内核再攒一会儿。
void set_tcp_nodelay(int fd) {
    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}


// ==================== 创建监听 socket ====================
// 绑定所有网卡上的 port。backlog 是已完成握手、还没被 accept 取走的连接能排多深。失败返回 -1。
int make_listen_socket(int port, int backlog) {
    int sock = socket(PF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    int opt = 1;
    // 端口还在 TIME_WAIT 时也允许重新绑定，进程重启后能立刻再听
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
