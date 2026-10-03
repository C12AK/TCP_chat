#ifndef NET_H
#define NET_H

#include "protocol.h"

#include <cstddef>
#include <string>

// 阻塞式完整发送（调试客户端用）。失败抛出 std::runtime_error
void blocking_send_all(int sock, const char* data, std::size_t len);

// 阻塞式接收一帧。返回 1 成功，0 对端关闭，-1 错误
int blocking_recv_frame(int sock, std::string& acc, Frame& out);

// 将 fd 设为非阻塞
void set_nonblocking(int fd);

// 关闭 Nagle，小包立即发出
void set_tcp_nodelay(int fd);

// 创建并监听 socket（SO_REUSEADDR，IPv4 any）。失败返回 -1
int make_listen_socket(int port, int backlog);

#endif // NET_H
