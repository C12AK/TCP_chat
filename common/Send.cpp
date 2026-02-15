#include <iostream>
#include <sys/socket.h>
#include <cerrno>
#include <unistd.h>

#define MAX_RETRIES 100     // 最大重试次数


void Send(int sock, const char* sp, int len) {
    int sent = 0, retries = 0;
    while (sent < len) {
        int n = send(sock, sp + sent, len - sent, MSG_NOSIGNAL);  // 禁止 SIGPIPE ，而是返回 -1 且 errno = EPIPE
        if (n <= 0) {
            if ((errno == EAGAIN || errno == EWOULDBLOCK) && retries < MAX_RETRIES) {
                retries++;
                usleep(1000);   // 发送缓冲区满则稍后重试。更好的实现是检测到 EPOLLOUT 时继续发送，但需要更复杂的整体架构
                continue;
            } else {
                perror("send of Send");
                return;
            }
        }
        sent += n;
        retries = 0;    // 发送成功一次后，重置重试次数
    }

    if (sent < len) {
        perror("in Send partially sent");
        return;
    }
}