#ifndef REACTOR_H
#define REACTOR_H

#include "db.h"
#include "protocol.h"
#include "thread_pool.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class Connection;

// 单线程收发。写库、查关系、系统消息封信都交给线程池。
class Reactor {
  public:
    Reactor(int port, Database& db, std::size_t workers, int idle_ms, int queue_cap);
    ~Reactor();

    Reactor(const Reactor&) = delete;
    Reactor& operator=(const Reactor&) = delete;

    void run();   // epoll 循环：接受连接、读帧、把线程池交回的帧发出去、清掉心跳超时

  private:
    // 线程池做完后投回本线程的结果。kind=0 只发送；1 登录成功；2 注册成功
    struct Action {
        int fd = -1;
        int gen = 0;
        int kind = 0;  // 0 发送帧  1 登录成功  2 注册成功
        std::string frame;
        uint64_t user_id = 0;
        std::string username;
    };

    struct Online {
        int fd = -1;
        int gen = 0;
    };

    int port;
    Database& db;
    int idle_ms;
    int queue_cap;

    int listen_sock = -1;
    int epfd = -1;
    int wake_fd = -1;

    ThreadPool pool;
    std::unordered_map<int, std::unique_ptr<Connection>> conns;
    int next_gen = 1; // 下一条新连接的代次，close 后 fd 号会复用，靠它区分

    std::mutex online_mu;
    std::unordered_map<uint64_t, Online> online; // 用户 id → 当前这条已登录连接

    std::mutex queue_mu;
    std::unordered_map<uint64_t, int> inflight; // 该用户还没写完的任务数
    std::mutex rev_mu;
    std::unordered_map<uint64_t, uint64_t> list_rev; // 该用户下一份会话列表的版本，只增

    std::mutex actions_mu;
    std::deque<Action> actions;

    void on_accept();                                              // 新连接，尚未登录
    void on_readable(Connection& c, uint32_t evs);                 // 读到字节后按帧交给 handle_frame
    void on_writable(Connection& c);                               // 发送缓冲有空位，继续 flush
    void on_wakeup();                                              // eventfd：取走线程池投回的 Action
    void sweep_idle();                                             // 超过 idle_ms 没有入站数据就断开

    void handle_frame(Connection& c, MsgType type, const std::string& payload);
    void enqueue_frame(Connection& c, std::string frame);          // 放入出站队列并尝试立刻 send
    void try_flush(Connection& c);                                 // 非阻塞 send。发完且状态为 Closing 则关闭
    void update_epoll_events(Connection& c);                       // 还有没发完的数据才监听 EPOLLOUT
    void close_conn(int fd, const char* reason);                   // 仅当 fd+gen 仍是该用户的在线连接时才摘掉在线表

    bool try_acquire(uint64_t user_id);                            // 该用户未完成的写库任务达到上限则拒绝
    void release_user(uint64_t user_id);
    void post_action(Action a);                                    // 任意线程调用。队列从空变非空才写一次 eventfd
    void send_frame_to(int fd, int gen, MsgType type, const std::string& payload);
    void send_to_user(uint64_t user_id, const std::string& frame); // 对方不在线则丢掉，等他 SyncReq 来拉

    std::string conv_list_frame(uint64_t user_id);                 // 带递增版本号，客户端丢掉较旧的一份
    void finish_login(Connection& c, uint64_t user_id, const std::string& username); // 顶掉旧连接，下发 LoginOk 和会话列表
    void notify_system(uint64_t user_id, const std::string& text); // 用对方公钥封一条系统说明并落库、推送
};

#endif
