#include <iostream>
#include <cstring>
#include <format>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>

#define BUFSZ 1024

// 发送消息
inline void send_msg(int sock, const std::string& to, const std::string& msg);

// 解析消息
inline void split_msg(const char* buf, int len, std::string& from, std::string& msg);

int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << std::format("Usage: {} <Server IP> <Server Port> <Username>", argv[0]) << std::endl;
        exit(1);
    }

    int sock = socket(PF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        exit(1);
    }

    sockaddr_in srv_addr{};
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_addr.s_addr = inet_addr(argv[1]);
    srv_addr.sin_port = htons(atoi(argv[2]));

    if (connect(sock, (sockaddr *)&srv_addr, sizeof(srv_addr)) < 0) {
        perror("connect");
        close(sock);
        exit(1);
    }
    std::cout << "Initializing, plz wait...\n" << std::endl;

    // 用户名发给服务器
    send(sock, argv[3], strlen(argv[3]), 0);

    fd_set fds;
    int mxfd = std::max(sock, fileno(stdin));
    char buf[BUFSZ];
    std::string from, to, msg;

    while (1) {
        // 重新初始化可读事件的文件描述符集合，应包含服务器消息和键盘输入
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        FD_SET(fileno(stdin), &fds);

        // 内核检查的范围是 [0, mxfd+1)；返回值是就绪（即变为可读）的文件描述符数量
        int ready = select(mxfd + 1, &fds, nullptr, nullptr, nullptr);
        if (ready < 0) {
            perror("select");
            close(sock);
            exit(1);
        }

        // 如果是有服务器消息
        if (FD_ISSET(sock, &fds)) {
            int len = recv(sock, buf, BUFSZ - 1, 0);

            // 如果服务器关闭或接收出错
            if (len == 0) {
                std::cout << "Server closed." << std::endl;
                break;
            }
            else if (len < 0) {
                perror("recv");
                break;
            }
            
            buf[len] = '\0';
            split_msg(buf, len, from, msg);
            std::cout << format("\n> {}:\n> {}\n", from, msg) << std::endl;
        }

        // 如果是键盘有输入
        if (FD_ISSET(fileno(stdin), &fds)) {
            if (!std::getline(std::cin, msg) || msg == ".exit") break;

            // 没设收件人则设置
            if (!to.length()) to = msg;

            else {
                send_msg(sock, to, msg);
                to = "";
                std::cout << "- SENT\n" << std::endl;
            }
        }
    }

    close(sock);
    std::cout << "Exited." << std::endl;
    return 0;
}

// 消息格式：发送/接收方长度 (2B) + [发送/接收方 + 消息内容 + '\0'] (至多共 1022 B)
inline void send_msg(int sock, const std::string& to, const std::string& msg) {
    uint16_t tolen = htons(static_cast<uint16_t>(to.length()));
    std::string pck;
    pck.reserve(sizeof(tolen) + to.length() + msg.length());
    pck.append(reinterpret_cast<const char*>(&tolen), sizeof(tolen));
    pck += to, pck += msg;
    send(sock, pck.c_str(), pck.length(), 0);
}

inline void split_msg(const char* buf, int len, std::string& from, std::string& msg) {
    uint16_t n_fromlen;
    std::memcpy(&n_fromlen, buf, sizeof(n_fromlen));
    int fromlen = ntohs(n_fromlen);

    from = std::string(buf + 2, fromlen);
    msg = std::string(buf + 2 + fromlen, len - (2 + fromlen));
}