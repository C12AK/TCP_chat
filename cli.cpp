#include <bits/stdc++.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>

#define BUFSZ 1024

inline void split_msg(const char* buf, int len, std::string& from, std::string& msg);

int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << std::format("Usage: {} <Server IP> <Server Port> <Username>\n", argv[0]);
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
        exit(1);
    }
    std::cout << "Connected to server.\n";
    std::cout << "Usage: <Target user>(Line 1) + <Message>(Line 2)\n";
    std::cout << "Input \".exit\"(without quotes) at any time to exit.\n" << std::endl;

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
            exit(1);
        }

        // 如果是有服务器消息
        if (FD_ISSET(sock, &fds)) {
            int len = recv(sock, buf, BUFSZ - 1, 0);
            if (len <= 0) break;
            buf[len] = '\0';
            split_msg(buf, len, from, msg);
            std::cout << format("\n> {}:\n> {}\n", from, msg) << std::endl;
        }

        // 如果是键盘有输入
        if (FD_ISSET(fileno(stdin), &fds)) {
            if (!std::getline(std::cin, msg) || msg == ".exit") break;

            // 没设收件人则设置
            if (!to.length()) to = msg;

            // 否则拼接消息：【收件人长度】#【收件人】【消息内容】
            else {
                msg = std::to_string(to.length()) + "#" + to + msg;
                send(sock, msg.c_str(), msg.length(), 0);
                to = "";
                std::cout << "- SENT\n" << std::endl;
            }
        }
    }

    close(sock);
    std::cout << "Exited." << std::endl;
    return 0;
}

inline void split_msg(const char* buf, int len, std::string& from, std::string& msg) {
    int i{}, tolen{};
    while (i < len && buf[i] != '#') {
        tolen = tolen * 10 + (buf[i] - '0');
        i++;
    }
    i++;
    from = std::string(buf + i, tolen);
    i += tolen;
    msg = std::string(buf + i, len - i);
}