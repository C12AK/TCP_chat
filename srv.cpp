#include <iostream>
#include <vector>
#include <unordered_map>
#include <string>
#include <cstring>
#include <format>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <cerrno>

#define BUFSZ 1024
#define MAX_EVENTS 1024

std::unordered_map<std::string, int> usr2sock;
std::unordered_map<int, std::string> sock2usr;

// 设置 socket 为非阻塞
inline void set_nonblocking(int fd);

// 解析消息：【发件人长度】#【发件人】【消息内容】
inline void split_msg(const char* buf, int len, std::string& to, std::string& msg);

// 发送消息
inline void send_msg(int fd, const std::string& from, const std::string& msg);

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << std::format("Usage: {} <Port>\n", argv[0]);
        exit(1);
    }
    int port = atoi(argv[1]);

    int listen_sock = socket(PF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) {
        perror("socket");
        exit(1);
    }

    // 允许重用 TIME_WAIT 的本地地址
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(listen_sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_sock);
        exit(1);
    }

    if (listen(listen_sock, 10) < 0) {
        perror("listen");
        close(listen_sock);
        exit(1);
    }

    // 创建 epoll 实例
    int epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("epoll_create1");
        close(listen_sock);
        exit(1);
    }

    // 构造要注册到 epoll 的事件结构
    epoll_event ev;
    ev.events = EPOLLIN;            // 关注可读事件
    ev.data.fd = listen_sock;

    // 将监听 socket 加入 epoll 监控
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_sock, &ev) < 0) {
        perror("epoll_ctl listen_sock");
        close(epfd);
        close(listen_sock);
        exit(1);
    }

    std::cout << std::format("Server started on port {}", port) << std::endl;

    std::vector<epoll_event> events(MAX_EVENTS);
    char buf[BUFSZ];

    while (1) {
        // 等待事件发生，-1表示无事件时永久阻塞，返回就绪事件数量
        int nfds = epoll_wait(epfd, events.data(), MAX_EVENTS, -1);

        if (nfds < 0) {
            if (errno == EINTR) continue;       // 被信号中断，重试
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;

            // 如果有新连接
            if (fd == listen_sock) {
                sockaddr_in cli_addr;
                socklen_t cli_len = sizeof(cli_addr);

                int cli_sock = accept(listen_sock, (sockaddr*)&cli_addr, &cli_len);
                if (cli_sock < 0) {
                    perror("accept");
                    continue;
                }

                // 立即接收用户名
                int len = recv(cli_sock, buf, BUFSZ - 1, 0);

                // 如果刚连接就断开
                if (len == 0) {
                    std::cout << std::format("New connection closed on accepting: {}:{}", 
                        inet_ntoa(cli_addr.sin_addr), ntohs(cli_addr.sin_port)) << std::endl;
                    close(cli_sock);
                    continue;
                }

                // 如果出错
                else if (len < 0) {
                    perror("recv");
                    close(cli_sock);
                    continue;
                }

                buf[len] = '\0';

                // 用户名已存在则报错并关闭连接
                if (usr2sock.find(buf) != usr2sock.end()) {
                    send_msg(cli_sock, "Server", std::format("Username {} already in use.", buf));
                    std::cout << std::format("Rejected {}:{}, Duplicate username {}", 
                        inet_ntoa(cli_addr.sin_addr), ntohs(cli_addr.sin_port), buf) << std::endl;
                    close(cli_sock);
                    continue;
                }

                usr2sock[buf] = cli_sock, sock2usr[cli_sock] = buf;

                // 防止后续 recv 阻塞整个线程
                set_nonblocking(cli_sock);

                epoll_event ev;
                ev.events = EPOLLIN;
                ev.data.fd = cli_sock;

                if (epoll_ctl(epfd, EPOLL_CTL_ADD, cli_sock, &ev) < 0) {
                    perror("epoll_ctl add client");
                    close(cli_sock);
                    continue;
                }

                std::cout << std::format("New connection: {}:{}, Username: {}", 
                    inet_ntoa(cli_addr.sin_addr), ntohs(cli_addr.sin_port), buf) << std::endl;
                continue;
            }

            // 如果已连接的客户端有可读数据
            if (events[i].events & EPOLLIN) {
                int len = recv(fd, buf, BUFSZ - 1, 0);

                // 如果连接关闭或出错
                if (len <= 0) {
                    std::string usr = sock2usr[fd];

                    if (len == 0)
                        std::cout << std::format("Client disconnected: {}", usr) << std::endl;
                    else
                        perror("recv");
                    
                    usr2sock.erase(usr), sock2usr.erase(fd);
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);        // 移除 epoll 监控
                    close(fd);
                    continue;
                }

                buf[len] = '\0';
                std::string from = sock2usr[fd], to, msg;
                split_msg(buf, len, to, msg);
                std::cout << std::format("\nFrom: {}\nTo: {}\nContent: {}\n", 
                    from, to, msg) << std::endl;

                auto it = usr2sock.find(to);

                // 如果目标用户不存在，给发送方报错
                if (it == usr2sock.end())
                    send_msg(fd, "Server", std::format("User {} does not exist.", to));
                else
                    send_msg(it->second, from, msg);
            }
        }
    }

    close(epfd);
    close(listen_sock);
    return 0;
}

inline void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

inline void split_msg(const char* buf, int len, std::string& to, std::string& msg) {
    int i{}, tolen{};
    while (i < len && buf[i] != '#') {
        tolen = tolen * 10 + (buf[i] - '0');
        i++;
    }
    i++;
    to = std::string(buf + i, tolen);
    i += tolen;
    msg = std::string(buf + i, len - i);
}

inline void send_msg(int fd, const std::string& from, const std::string& msg) {
    std::string s = std::to_string(from.length()) + "#" + from + msg;
    send(fd, s.c_str(), s.length(), 0);
}