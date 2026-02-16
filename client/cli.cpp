#include "crypto.h"

#include <iostream>
#include <cstring>
#include <format>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <stdexcept>

#define BUFSZ 1024

Crypto crypto{};


// ==================== 工具函数 ====================
inline void send_msg(int sock, const std::string& to, const std::string& msg);  // 发送消息
void Send(int sock, const char* sp, int len);

inline void process_msg(const char* buf, int len, std::string& from, std::string& msg); // 拆解消息


// ==================== 主函数 ====================
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

    char buf[BUFSZ];
    send(sock, argv[3], strlen(argv[3]), MSG_NOSIGNAL);     // 用户名发给服务器
    int len = recv(sock, buf, BUFSZ - 1, 0);                // 服务端收到后会发送 RSA 公钥
    if (len == 0) {
        std::cout << "Server closed." << std::endl;
        close(sock);
        exit(1);
    } else if (len < 0) {
        perror("recv");
        close(sock);
        exit(1);
    }
    vecuc pubkey_der(buf, buf + len);                           // 收到 RSA 公钥

    try {
        crypto.set_pubkey_from_der(pubkey_der);                 // 设置 RSA 公钥
    } catch (const std::exception& e) {
        std::cerr << "Set RSA pubkey: " << e.what() << std::endl;
        close(sock);
        exit(1);
    }

    try{
        crypto.generate_rand_aeskey();                          // 生成 AES 密钥
    } catch (const std::exception& e) {
        std::cerr << "Generate AES key: " << e.what() << std::endl;
        close(sock);
        exit(1);
    }

    vecuc c_aeskey;
    try {
        c_aeskey = crypto.rsa_encrypt(crypto.aeskey);
    } catch (const std::exception& e) {
        std::cerr << "RSA encrypt: " << e.what() << std::endl;
        close(sock);
        exit(1);
    }
    send(sock, c_aeskey.data(), c_aeskey.size(), MSG_NOSIGNAL); // 发送加密的 AES 密钥

    fd_set fds;
    int mxfd = std::max(sock, fileno(stdin));
    std::string from, to, msg, recvbuf;
    int expected_len = -1;

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
            } else if (len < 0) {
                perror("recv");
                break;
            }

            buf[len] = '\0';
            recvbuf.append(buf, len);

            // 设置期望长度
            if (expected_len == -1 && recvbuf.length() >= 6ul) {
                uint16_t n_fromlen;
                uint32_t n_msglen;
                memcpy(&n_fromlen, recvbuf.c_str(), sizeof(n_fromlen));
                memcpy(&n_msglen, recvbuf.c_str() + sizeof(n_fromlen), sizeof(n_msglen));
                expected_len = 6 + static_cast<int>(ntohs(n_fromlen)) + ntohl(n_msglen);
            }

            // 收到的消息长度够了才处理
            if (expected_len != -1 && recvbuf.length() >= expected_len) {
                std::string pck = recvbuf.substr(0, expected_len);
                recvbuf.erase(0, expected_len);
                expected_len = -1;

                process_msg(pck.c_str(), pck.length(), from, msg);
                std::cout << format("\n> {}:\n> {}\n", from, msg) << std::endl;
            }
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

    Send(sock, pck.c_str(), pck.length());
}


inline void process_msg(const char* pckptr, int len, std::string& from, std::string& msg) {
    if (len < 6) {
        from.clear(), msg.clear();
        return;
    }

    uint16_t n_fromlen;
    uint32_t n_msglen;
    std::memcpy(&n_fromlen, pckptr, sizeof(n_fromlen));
    std::memcpy(&n_msglen, pckptr + sizeof(n_fromlen), sizeof(n_msglen));
    int fromlen = ntohs(n_fromlen), msglen = ntohl(n_msglen);

    if (fromlen < 0 || msglen < 0 || 6 + fromlen + msglen > len) {
        from.clear(), msg.clear();
        return;
    }

    try {
        from = crypto.aes_decrypt(std::string(pckptr + 6, fromlen));
        msg = crypto.aes_decrypt(std::string(pckptr + 6 + fromlen, msglen));
    } catch (const std::exception& e) {
        std::cerr << "AES decrypt: " << e.what() << std::endl;
        from.clear(), msg.clear();
    }
}