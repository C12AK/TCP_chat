#ifndef REACTOR_H
#define REACTOR_H

#include "connection.h"
#include "registry.h"
#include "thread_pool.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>


// 单线程 Reactor：独占监听 socket、epoll 与所有连接的 I/O 缓冲。
// CPU 密集的 AES 加解密与目标查询交给 ThreadPool。
// 工作线程产出的转发帧以 Action 形式投递回 Reactor，经 eventfd 唤醒
class Reactor {
  public:
    Reactor(int port, std::size_t worker_num, int handshake_timeout_ms);
    ~Reactor();

    Reactor(const Reactor&) = delete;
    Reactor& operator=(const Reactor&) = delete;

    void run();

  private:
    // 工作线程 -> Reactor 的动作：把 payload 追加到 {fd, gen} 对应连接的出站队列
    struct Action {
        int fd;
        int gen;
        std::string payload;
    };

    // 握手超时最小堆条目
    struct TimerEntry {
        int64_t deadline_ms;
        int fd;
        int gen;
    };
    struct TimerCmp {
        bool operator()(const TimerEntry& a, const TimerEntry& b) const {
            return a.deadline_ms > b.deadline_ms;
        }
    };

    // ---- 构造参数 ----
    int port;
    int handshake_timeout_ms;

    // ---- 资源 ----
    int listen_sock = -1;
    int epfd = -1;
    int wake_fd = -1;     // eventfd ，唤醒 epoll_wait 去消费 actions

    ThreadPool pool;
    Registry registry;

    // ---- Reactor 线程独占 ----
    std::unordered_map<int, std::unique_ptr<Connection>> conns;
    int next_gen = 1;
    std::priority_queue<TimerEntry, std::vector<TimerEntry>, TimerCmp> timers;

    // ---- 跨线程 ----
    std::mutex actions_mtx;
    std::deque<Action> actions;

    // ---- 事件分发 ----
    void on_accept();
    void on_readable(int fd, uint32_t evs);
    void on_writable(int fd);
    void on_wakeup();
    void sweep_timers();

    // ---- 协议推进 ----
    void try_advance_handshake(Connection& c);
    void try_parse_chat_packets(Connection& c);

    // ---- 出站 ----
    void enqueue_frame(Connection& c, std::string frame);
    void try_flush(Connection& c);
    void update_epoll_events(Connection& c);

    // ---- 关闭 ----
    void close_conn(int fd, const char* reason);

    // ---- 由 worker 调用 ----
    void post_action(Action a);
};

#endif // REACTOR_H
