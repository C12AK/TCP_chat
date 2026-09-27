#include "aes.h"
#include "common.h"
#include "crypto.h"
#include "net.h"
#include "protocol.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>


#define READ_CHUNK 4096


// ==================== 握手 ====================
// 完成 ECDH 握手，返回派生得到的 AES 密钥。失败则抛出异常
static vecuc do_handshake(int sock, const char* username) {
    std::string frame = build_ka_frame(std::string(username));
    blocking_send_all(sock, frame.data(), frame.size());

    vecuc srv_pub;
    int n = blocking_recv_ka_frame(sock, srv_pub);
    if (n == 0) throw std::runtime_error("server closed");
    if (n < 0) throw std::runtime_error("recv server pubkey failed");

    Crypto crypto;
    crypto.generate_ecdh_keypr();
    vecuc cli_pub = crypto.get_ecdh_pubkey();
    crypto.set_peer_ecdh_pubkey(srv_pub);
    crypto.derive_shared_secret(&FIXED_SALT);

    std::string frame2 = build_ka_frame(cli_pub);
    blocking_send_all(sock, frame2.data(), frame2.size());

    return std::move(crypto.aeskey);
}


// ==================== 主函数 ====================
int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << std::format("Usage: {} <Server IP> <Server Port> <Username>", argv[0]) << std::endl;
        return 1;
    }
    if (std::strlen(argv[3]) > 500ul) {
        std::cerr << "Username can't be longer than 500 characters" << std::endl;
        return 1;
    }

    int sock = socket(PF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    sockaddr_in srv_addr{};
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_addr.s_addr = inet_addr(argv[1]);
    srv_addr.sin_port = htons(std::atoi(argv[2]));

    if (connect(sock, reinterpret_cast<sockaddr*>(&srv_addr), sizeof(srv_addr)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }
    set_tcp_nodelay(sock);
    std::cout << "Initializing, plz wait...\n" << std::endl;

    vecuc aeskey;
    try {
        aeskey = do_handshake(sock, argv[3]);
    } catch (const std::exception& e) {
        std::cerr << "Handshake: " << e.what() << std::endl;
        close(sock);
        return 1;
    }

    // ==================== 聊天主循环 ====================
    char buf[READ_CHUNK];
    std::string to, msg, recvbuf;
    fd_set fds;
    int mxfd = std::max(sock, fileno(stdin));

    while (true) {
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        FD_SET(fileno(stdin), &fds);

        int ready = select(mxfd + 1, &fds, nullptr, nullptr, nullptr);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("select");
            close(sock);
            return 1;
        }

        // ---- 服务器消息 ----
        if (FD_ISSET(sock, &fds)) {
            int n = recv(sock, buf, sizeof(buf), 0);
            if (n == 0) { std::cout << "Server closed." << std::endl; break; }
            if (n < 0) { perror("recv"); break; }
            recvbuf.append(buf, n);

            try {
                while (auto f = parse_chat_frame(recvbuf)) {
                    std::string from = aes_decrypt(aeskey, f->c_to);
                    std::string content = aes_decrypt(aeskey, f->c_msg);
                    std::cout << std::format("\n> {}:\n> {}\n", from, content) << std::endl;
                }
            } catch (const std::exception& e) {
                std::cerr << "Recv: " << e.what() << std::endl;
                break;
            }
        }

        // ---- 键盘输入 ----
        if (FD_ISSET(fileno(stdin), &fds)) {
            if (!std::getline(std::cin, msg) || msg == ".exit") break;

            if (to.empty()) {
                to = msg;
            } else {
                try {
                    std::string frame = build_chat_frame(
                        aes_encrypt(aeskey, to),
                        aes_encrypt(aeskey, msg));
                    blocking_send_all(sock, frame.data(), frame.size());
                } catch (const std::exception& e) {
                    std::cerr << "Send: " << e.what() << std::endl;
                }
                to.clear();
                std::cout << "- SENT\n" << std::endl;
            }
        }
    }

    close(sock);
    std::cout << "Exited." << std::endl;
    return 0;
}
