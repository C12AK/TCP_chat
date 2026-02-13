#include <iostream>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <queue>
#include <format>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <cerrno>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
#include <condition_variable>
#include <memory>

#define BUFSZ 1024
#define MAX_EVENTS 1024


// ==================== 线程池 ====================
class ThreadPool {
  private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> task_queue;
    std::mutex queue_mtx;
    std::condition_variable cv;
    std::atomic<bool> stop;

  public:
    // 启动 thread_num 个工作线程
    explicit ThreadPool(size_t thread_num) : stop(false) {
        for (size_t i = 0; i < thread_num; ++i) {
            workers.emplace_back([this] {
                while (1) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mtx);
                        this->cv.wait(lock, [this] { return this->stop || !this->task_queue.empty(); });    // 等待，要退出了或者有任务时才唤醒
                        if (this->stop && this ->task_queue.empty()) return;                                // 执行完所有任务才能退出
                        task = std::move(this->task_queue.front());
                        this->task_queue.pop();
                    }
                    task();
                }
            });
        }
    }

    // 将任意可调用对象加入任务队列
    template<class F>
    void enqueue(F&& f) {
        {
            std::unique_lock<std::mutex> lock(queue_mtx);
            if (stop) throw std::runtime_error("enqueue on stopped ThreadPool");
            task_queue.emplace(std::forward<F>(f)); // 完美转发
        }
        cv.notify_one();
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mtx);
            stop = true;
        }
        cv.notify_all();    // 唤醒所有等待中的工作线程，让它们检测 stop 并退出
        for (std::thread& worker : workers) worker.join();
    }
};


// ==================== 全局变量 ====================
std::unordered_map<std::string, int> usr2sock;
std::unordered_map<int, std::string> sock2usr;
std::mutex cli_map_mtx;

std::unordered_map<int, std::string> pcks;  // 存储已经收到的消息
std::unordered_map<int, int> expected_len;
std::mutex pcks_mtx;

std::unordered_map<int, std::unique_ptr<std::mutex>> fd_mtxs;   // 每个 fd 设一个锁，防止多个线程同时对同一个 fd send/recv
std::mutex fdmtx_map_mtx;

thread_local char buf[BUFSZ];   // 每个线程一份，收发消息的缓冲区


// ==================== 工具函数 ====================
// 将 fd 设为非阻塞
inline void set_nonblocking(int fd);

// 提交发送任务到线程池
void submit_send_task(ThreadPool& pool, int fd, const std::string& from, const std::string& msg);

// 和 submit_send_task() 几乎一样 ([2]) ，专用于拒绝用户名已使用的连接
void submit_send_task_reject(ThreadPool& pool, int fd, const std::string& from, const std::string& msg);

// 组装并发送消息
void send_msg(int fd, const std::string& from, const std::string& msg);

// 循环发送任意长字符串
void Send(int sock, const char* sp, int len);

// 拆解收到的消息
void process_msg(const char* buf, int len, std::string& to, std::string& msg);

// 移除一个用户
inline void rm_usr(int sock, const std::string& usr);


// ==================== 主函数 ====================
int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << std::format("Usage: {} <Port>", argv[0]) << std::endl;
        exit(1);
    }
    int port = atoi(argv[1]);

    int listen_sock = socket(PF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) {
        perror("socket");
        exit(1);
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));   // 允许重用地址，避免 TIME_WAIT 问题

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

    int epfd = epoll_create1(0);    // 创建 epoll 实例
    if (epfd < 0) {
        perror("epoll_create1");
        close(listen_sock);
        exit(1);
    }

    epoll_event ev{};
    ev.events = EPOLLIN;        // 关注可读数据
    ev.data.fd = listen_sock;   // 简单地用 fd 作为用户数据

    // 注册监听 socket 到 epoll
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_sock, &ev) < 0) {
        perror("epoll_ctl add listen_sock");
        close(epfd);
        close(listen_sock);
        exit(1);
    }

    std::cout << std::format("Server started on port {}", port) << std::endl;

    ThreadPool pool(std::thread::hardware_concurrency());   // 创建线程池，使用硬件支持的并发数

    std::vector<epoll_event> events(MAX_EVENTS);    // 为就绪事件准备的缓冲区

    while (1) {
        int nfds = epoll_wait(epfd, events.data(), MAX_EVENTS, -1); // 等待事件到来，永久阻塞直到有事件
        if (nfds < 0) {
            if (errno == EINTR) continue;   // 被信号中断，重试
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;
            uint32_t evs = events[i].events;

            // 1. 如果有新连接
            if (fd == listen_sock) {
                sockaddr_in cli_addr;
                socklen_t cli_addr_len = sizeof(cli_addr);
                int cli_sock = accept(listen_sock, (sockaddr*)&cli_addr, &cli_addr_len);
                if (cli_sock < 0) {
                    perror("accept");
                    continue;
                }

                // 立即处理新连接用户名。按目前的处理流程，这段可以提交到线程池，但容易逻辑混乱
                int len = recv(cli_sock, buf, BUFSZ - 1, 0);
                if (len <= 0) {
                    std::cout << std::format("New connection closed on accepting: {}:{}", 
                        inet_ntoa(cli_addr.sin_addr), ntohs(cli_addr.sin_port)) << std::endl;
                    close(cli_sock);
                    continue;
                }
                buf[len] = '\0';
                std::string username(buf, len);

                bool dupf = false;
                {
                    std::lock_guard<std::mutex> lock(cli_map_mtx);
                    if (usr2sock.find(username) != usr2sock.end()) {
                        dupf = true;                                    // 如果用户名已被占用，接下来处理
                    } else {
                        usr2sock[username] = cli_sock;                  // 未占用则记录
                        sock2usr[cli_sock] = username;
                        {
                            std::lock_guard<std::mutex> lock2(pcks_mtx);     // 加锁清空已有消息
                            pcks[cli_sock].clear();
                            expected_len[cli_sock] = -1;
                        }
                    }
                }

                if (dupf) {
                    submit_send_task_reject(pool, cli_sock, "Server", 
                        std::format("Username {} already in use.", username));          // 通知用户：拒绝
                    std::cout << std::format("Rejected {}:{}, Duplicate username {}", 
                        inet_ntoa(cli_addr.sin_addr), ntohs(cli_addr.sin_port), username) << std::endl;
                    continue;
                }

                set_nonblocking(cli_sock);  // 这里才设置非阻塞

                epoll_event ev{};
                ev.events = EPOLLIN | EPOLLRDHUP;   // 对于客户 socket ，关注可读 + 对端关闭写端
                ev.data.fd = cli_sock;
                if (epoll_ctl(epfd, EPOLL_CTL_ADD, cli_sock, &ev) < 0) {
                    perror("epoll_ctl add client");
                    {
                        std::lock_guard<std::mutex> lock(cli_map_mtx);
                        rm_usr(cli_sock, username);
                    }
                    continue;
                }

                submit_send_task(pool, cli_sock, "Server", 
                    "\tConnected to server.\n"
                    "\tUsage: <Target user>(Line 1) + <Message>(Line 2)\n"
                    "\tInput \".exit\"(without quotes) at any time to exit.");            // 通知用户：已连接
                std::cout << std::format("New connection: {}:{}, Username: {}", 
                    inet_ntoa(cli_addr.sin_addr), ntohs(cli_addr.sin_port), username) << std::endl;
                continue;
            }

            // 2. 如果对端发生错误 / 挂起 / 写端关闭
            if (evs & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                std::string usr;
                {
                    std::lock_guard<std::mutex> lock(cli_map_mtx);
                    auto it = sock2usr.find(fd);
                    if (it != sock2usr.end()) {
                        usr = it->second;
                        rm_usr(fd, usr);                        // [1]
                    } else {
                        close(fd);
                        continue;
                    }
                }
                if (!usr.empty()) {
                    if (evs & EPOLLRDHUP) {
                        std::cout << std::format("Client {} closed connection", usr) << std::endl;
                    } else {
                        std::cerr << std::format("Client {} error or hangup", usr) << std::endl;
                    }
                }

                epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);    // 实际上 [1] 处 close(fd) 已经自动从 epoll 移除，这里显式写出来
                continue;
            }

            // 3. 如果有可读数据
            if (evs & EPOLLIN) {
                int len = recv(fd, buf, BUFSZ - 1, 0);  // 读写缓冲区分离，主线程 recv 不用加锁

                // 理论上 len = 0 已经被上面 EPOLLRDHUP 检测到
                if (len <= 0) {
                    std::string usr;
                    {
                        std::lock_guard<std::mutex> lock(cli_map_mtx);
                        auto it = sock2usr.find(fd);
                        if (it != sock2usr.end()) {
                            usr = it->second;
                            rm_usr(fd, usr);
                        } else {
                            close(fd);
                        }
                    }
                    if (len == 0 && !usr.empty()) {
                        std::cout << std::format("Client {} closed connection", usr) << std::endl;
                    }
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                    continue;
                }

                buf[len] = '\0';

                std::string pck;
                {
                    std::lock_guard<std::mutex> lock(pcks_mtx);
                    pcks[fd].append(buf, len);

                    // 设置期望长度
                    if (expected_len[fd] == -1 && pcks[fd].length() >= 6ul) {
                        uint16_t n_tolen;
                        uint32_t n_msglen;
                        memcpy(&n_tolen, pcks[fd].c_str(), sizeof(n_tolen));
                        memcpy(&n_msglen, pcks[fd].c_str() + sizeof(n_tolen), sizeof(n_msglen));
                        expected_len[fd] = 6 + static_cast<int>(ntohs(n_tolen)) + ntohl(n_msglen);
                    }

                    // 已经存在一个完整的包，就处理
                    if (expected_len[fd] != -1 && pcks[fd].length() >= expected_len[fd]) {
                        pck = pcks[fd].substr(0, expected_len[fd]);
                        pcks[fd].erase(0, expected_len[fd]);
                        expected_len[fd] = -1;
                    }
                }
                if (pck.empty()) continue;

                // 查找发送方用户名
                std::string from;
                {
                    std::lock_guard<std::mutex> lock(cli_map_mtx);
                    auto it = sock2usr.find(fd);
                    if (it == sock2usr.end()) {
                        close(fd);
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                        continue;
                    }
                    from = it->second;
                }

                // 提交消息处理任务到线程池
                pool.enqueue([pck = std::move(pck), from = std::move(from), fd, epfd, &pool]() {
                    std::string to, msg;
                    process_msg(pck.c_str(), pck.length(), to, msg);

                    int tofd = -1;
                    {
                        std::lock_guard<std::mutex> lock(cli_map_mtx);
                        auto it = usr2sock.find(to);
                        if (it != usr2sock.end()) tofd = it->second;
                    }

                    if (tofd == -1) {
                        // 其实单线程 Reactor 最好将 send 和 recv 全部放到主线程，但已经用线程池实现了，且逻辑正确
                        submit_send_task(pool, fd, "Server", "No such user.");
                        std::cout << std::format("\nFrom: {}\nTo: {} (No such user)\nContent: {}\n", 
                            from, to, msg) << std::endl;
                    } else {
                        submit_send_task(pool, tofd, from, msg);
                        std::cout << std::format("\nFrom: {}\nTo: {}\nContent: {}\n", 
                            from, to, msg) << std::endl;
                    }
                });
            }
        }
    }

    close(epfd);
    close(listen_sock);
    return 0;
}


// ==================== 工具函数实现 ====================
inline void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}


void submit_send_task(ThreadPool& pool, int fd, const std::string& from, const std::string& msg) {
    std::shared_ptr<std::mutex> mtx_ptr;    // 在线程任务中安全持有 mutex 的指针，即使 fd 被关闭，只要任务还在执行，mutex 就不会被销毁

    // 查找当前 fd 的mutex
    {
        std::lock_guard<std::mutex> lock(fdmtx_map_mtx);
        auto it = fd_mtxs.find(fd);
        if (it == fd_mtxs.end()) {
            auto new_mtx = std::make_unique<std::mutex>();                              // 当前 fd 没有 mutex ，新建一个

            // 通过自定义 deleter 为空的 shared_ptr 借用指针，delete 仍然由 unique_ptr 负责
            mtx_ptr = std::shared_ptr<std::mutex>(new_mtx.get(), [](std::mutex*){});

            // unique_ptr new_mtx 被转移给 fd_mtxs[fd] ，delete 也就由 fd_mtxs 存的这一项负责了
            fd_mtxs[fd] = std::move(new_mtx);
        } else {
            mtx_ptr = std::shared_ptr<std::mutex>(it->second.get(), [](std::mutex*){}); // 使用已有的 mutex
        }
    }

    pool.enqueue([fd, from = std::move(from), msg = std::move(msg), mtx_ptr]() {
        std::lock_guard<std::mutex> lock(*mtx_ptr);
        send_msg(fd, from, msg);
    });
}


void submit_send_task_reject(ThreadPool& pool, int fd, const std::string& from, const std::string& msg) {
    std::shared_ptr<std::mutex> mtx_ptr;
    {
        std::lock_guard<std::mutex> lock(fdmtx_map_mtx);
        auto it = fd_mtxs.find(fd);
        if (it == fd_mtxs.end()) {
            auto new_mtx = std::make_unique<std::mutex>();
            mtx_ptr = std::shared_ptr<std::mutex>(new_mtx.get(), [](std::mutex*){});
            fd_mtxs[fd] = std::move(new_mtx);
        } else {
            mtx_ptr = std::shared_ptr<std::mutex>(it->second.get(), [](std::mutex*){});
        }
    }
    pool.enqueue([fd, from = std::move(from), msg = std::move(msg), mtx_ptr]() {
        std::lock_guard<std::mutex> lock(*mtx_ptr);
        send_msg(fd, from, msg);
        close(fd);                  // [2] 仅在这里追加
    });
}


void send_msg(int fd, const std::string& from, const std::string& msg) {
    uint16_t n_fromlen = htons(static_cast<uint16_t>(from.length()));
    uint32_t n_msglen = htonl(static_cast<uint32_t>(msg.length()));

    std::string pck;
    pck.reserve(sizeof(n_fromlen) + sizeof(n_msglen) + from.length() + msg.length());

    pck.append(reinterpret_cast<const char*>(&n_fromlen), sizeof(n_fromlen));
    pck.append(reinterpret_cast<const char*>(&n_msglen), sizeof(n_msglen));
    pck += from, pck += msg;

    Send(fd, pck.c_str(), pck.length());
}


void process_msg(const char* pckptr, int len, std::string& to, std::string& msg) {
    if (len < 6) {
        to.clear(), msg.clear();
        return;
    }

    uint16_t n_tolen;
    uint32_t n_msglen;
    std::memcpy(&n_tolen, pckptr, sizeof(n_tolen));
    std::memcpy(&n_msglen, pckptr + sizeof(n_tolen), sizeof(n_msglen));
    int tolen = ntohs(n_tolen), msglen = ntohl(n_msglen);

    if (tolen < 0 || msglen < 0 || 6 + tolen + msglen > len) {
        to.clear(), msg.clear();
        return;
    }

    to = std::string(pckptr + 6, tolen);
    msg = std::string(pckptr + 6 + tolen, msglen);
}


inline void rm_usr(int sock, const std::string& usr) {
    usr2sock.erase(usr);    // 保证每次调用时 cli_map_mtx 都已经上锁
    sock2usr.erase(sock);
    {
        std::lock_guard<std::mutex> lock(pcks_mtx);
        pcks.erase(sock);
        expected_len.erase(sock);
    }
    {
        std::lock_guard<std::mutex> lock(fdmtx_map_mtx);
        fd_mtxs.erase(sock);
    }
    close(sock);
}