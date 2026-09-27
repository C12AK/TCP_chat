#include "reactor.h"
#include "aes.h"
#include "net.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <iostream>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>


#define MAX_EVENTS 1024
#define READ_CHUNK 4096
#define OFFLOAD_CIPHER_BYTES 4096   // 密文合计超过此值才丢给线程池


// ==================== 日志（跨线程） ====================
namespace {

std::mutex log_mtx;

void log_out(const std::string& s) {
    std::lock_guard lock(log_mtx);
    std::cout << s << std::endl;
}

void log_err(const std::string& s) {
    std::lock_guard lock(log_mtx);
    std::cerr << s << std::endl;
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// epoll data.ptr 用这两个地址区分 listen / eventfd，其余为 Connection*
char listen_tag;
char wake_tag;

}   // namespace


// ==================== 构造 / 析构 ====================
Reactor::Reactor(int port, std::size_t worker_num, int handshake_timeout_ms)
    : port(port), handshake_timeout_ms(handshake_timeout_ms), pool(worker_num) {
    listen_sock = make_listen_socket(port, 4096);
    if (listen_sock < 0) {
        throw std::runtime_error(std::format("make_listen_socket: {}", std::strerror(errno)));
    }
    set_nonblocking(listen_sock);

    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) throw std::runtime_error(std::format("epoll_create1: {}", std::strerror(errno)));

    wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd < 0) throw std::runtime_error(std::format("eventfd: {}", std::strerror(errno)));

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.ptr = &listen_tag;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_sock, &ev);

    ev.data.ptr = &wake_tag;
    epoll_ctl(epfd, EPOLL_CTL_ADD, wake_fd, &ev);
}


Reactor::~Reactor() {
    if (wake_fd >= 0) close(wake_fd);
    if (listen_sock >= 0) close(listen_sock);
    // 关闭所有活跃连接
    for (auto& [fd, _] : conns) close(fd);
    if (epfd >= 0) close(epfd);
}


// ==================== 主循环 ====================
void Reactor::run() {
    log_out(std::format("Server started on port {}", port));
    std::vector<epoll_event> events(MAX_EVENTS);

    while (true) {
        // 根据最近的握手 deadline 选择 epoll_wait 超时
        int timeout = -1;
        while (!timers.empty()) {
            const auto& top = timers.top();
            auto it = conns.find(top.fd);
            if (it == conns.end() || it->second->gen != top.gen
                || it->second->state == ConnState::READY) {
                timers.pop();      // 已失效，清理后继续找堆顶
                continue;
            }
            int64_t diff = top.deadline_ms - now_ms();
            timeout = diff <= 0 ? 0 : static_cast<int>(diff);
            break;
        }

        int nfds = epoll_wait(epfd, events.data(), MAX_EVENTS, timeout);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            log_err(std::format("epoll_wait: {}", std::strerror(errno)));
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            void* p = events[i].data.ptr;
            uint32_t evs = events[i].events;

            if (p == &listen_tag) { on_accept(); continue; }
            if (p == &wake_tag) { on_wakeup(); continue; }

            auto* c = static_cast<Connection*>(p);
            int fd = c->fd;

            // 关键：必须先 recv 到 EOF / EAGAIN 再关闭，否则当客户端 send + close
            // 的两段报文合并为同一轮事件（EPOLLIN | EPOLLRDHUP）时，会丢弃残留数据
            if (evs & (EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP)) on_readable(*c, evs);
            if (evs & EPOLLOUT) {
                auto it = conns.find(fd);
                if (it != conns.end()) on_writable(*it->second);
            }
        }

        sweep_timers();
    }
}


// ==================== accept ====================
void Reactor::on_accept() {
    while (true) {
        sockaddr_in cli_addr{};
        socklen_t len = sizeof(cli_addr);
        int cli_sock = accept4(listen_sock, reinterpret_cast<sockaddr*>(&cli_addr), &len,
                               SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cli_sock < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            log_err(std::format("accept: {}", std::strerror(errno)));
            return;
        }

        set_tcp_nodelay(cli_sock);

        auto conn = std::make_unique<Connection>();
        conn->fd = cli_sock;
        conn->gen = next_gen++;
        conn->state = ConnState::WAIT_USERNAME;
        conn->cli_ip = inet_ntoa(cli_addr.sin_addr);
        conn->cli_port = ntohs(cli_addr.sin_port);
        conn->handshake_deadline_ms = now_ms() + handshake_timeout_ms;

        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLRDHUP;
        ev.data.ptr = conn.get();
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, cli_sock, &ev) < 0) {
            log_err(std::format("epoll_ctl add client: {}", std::strerror(errno)));
            close(cli_sock);
            continue;
        }

        timers.push({conn->handshake_deadline_ms, cli_sock, conn->gen});
        conns.emplace(cli_sock, std::move(conn));
    }
}


// ==================== 读事件 ====================
void Reactor::on_readable(Connection& c, uint32_t evs) {
    int fd = c.fd;
    bool peer_closed = false;
    char buf[READ_CHUNK];
    while (true) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            c.inbuf.append(buf, n);
            continue;
        }
        if (n == 0) { peer_closed = true; break; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        close_conn(fd, std::strerror(errno));
        return;
    }

    // 先把残留数据喂到状态机，再按需关闭
    try {
        if (c.state == ConnState::WAIT_USERNAME || c.state == ConnState::WAIT_CLI_PUBKEY) {
            try_advance_handshake(c);
        }
        if (c.state == ConnState::READY) try_parse_chat_packets(c);
    } catch (const std::exception& e) {
        log_err(std::format("conn[{}:{}] protocol error: {}", c.cli_ip, c.cli_port, e.what()));
        close_conn(fd, "protocol error");
        return;
    }

    if (peer_closed) {
        close_conn(fd, "EOF");
    } else if (evs & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
        close_conn(fd, (evs & EPOLLRDHUP) ? "closed by peer" : "hangup");
    }
}


// ==================== 握手推进 ====================
void Reactor::try_advance_handshake(Connection& c) {
    if (c.state == ConnState::WAIT_USERNAME) {
        auto pl = parse_ka_frame(c.inbuf, c.in_off);
        if (!pl) return;

        if (pl->size() > 500) throw std::runtime_error("username too long");
        c.username = std::move(*pl);

        // ECDH 密钥对生成 + 发送服务器公钥
        c.handshake_crypto.generate_ecdh_keypr();
        vecuc my_pub = c.handshake_crypto.get_ecdh_pubkey();
        enqueue_frame(c, build_ka_frame(my_pub));
        c.state = ConnState::WAIT_CLI_PUBKEY;
    }

    if (c.state == ConnState::WAIT_CLI_PUBKEY) {
        auto pl = parse_ka_frame(c.inbuf, c.in_off);
        if (!pl) return;

        vecuc peer_pub(pl->begin(), pl->end());
        c.handshake_crypto.set_peer_ecdh_pubkey(peer_pub);
        c.handshake_crypto.derive_shared_secret(&FIXED_SALT);

        auto key = std::make_shared<const vecuc>(std::move(c.handshake_crypto.aeskey));
        c.aeskey = key;

        // 注册用户名
        bool ok = registry.try_insert(c.username, Registry::Entry{c.fd, c.gen, key});
        if (!ok) {
            // 重名：排入拒绝消息，出站刷空后关闭
            std::string frame = build_chat_frame(
                aes_encrypt(*key, std::string("Server")),
                aes_encrypt(*key, std::format("Username {} already in use.", c.username)));
            enqueue_frame(c, std::move(frame));
            c.state = ConnState::CLOSING;
            log_out(std::format("Rejected {}:{}, Duplicate username {}", c.cli_ip, c.cli_port, c.username));
            return;
        }

        c.state = ConnState::READY;
        std::string welcome = build_chat_frame(
            aes_encrypt(*key, std::string("Server")),
            aes_encrypt(*key, std::string(
                "\tConnected to server.\n"
                "\tUsage: <Target user>(Line 1) + <Message>(Line 2)\n"
                "\tInput \".exit\"(without quotes) at any time to exit.")));
        enqueue_frame(c, std::move(welcome));
        log_out(std::format("New connection: {}:{}, Username: {}", c.cli_ip, c.cli_port, c.username));
    }

    compact_buf(c.inbuf, c.in_off);
}


// ==================== 聊天包解析并转发 ====================
void Reactor::try_parse_chat_packets(Connection& c) {
    while (true) {
        auto f = parse_chat_frame(c.inbuf, c.in_off);
        if (!f) break;

        int src_fd = c.fd;
        int src_gen = c.gen;
        auto src_key = c.aeskey;
        std::string from = c.username;
        std::string c_to = std::move(f->c_to);
        std::string c_msg = std::move(f->c_msg);

        const bool offload = (c_to.size() + c_msg.size()) >= OFFLOAD_CIPHER_BYTES;
        if (!offload) {
            Action a;
            if (build_action(*src_key, from, c_to, c_msg, src_fd, src_gen, a)) {
                emit_action(std::move(a));
            }
            continue;
        }

        pool.enqueue([this, src_fd, src_gen, src_key = std::move(src_key),
                      from = std::move(from), c_to = std::move(c_to),
                      c_msg = std::move(c_msg)]() mutable {
            Action a;
            if (build_action(*src_key, from, c_to, c_msg, src_fd, src_gen, a)) {
                post_action(std::move(a));
            }
        });
    }

    compact_buf(c.inbuf, c.in_off);
}


bool Reactor::build_action(const vecuc& src_key, const std::string& from,
                           const std::string& c_to, const std::string& c_msg,
                           int src_fd, int src_gen, Action& a) {
    std::string to, msg;
    try {
        to = aes_decrypt(src_key, c_to);
        msg = aes_decrypt(src_key, c_msg);
    } catch (const std::exception& e) {
        log_err(std::format("AES decrypt: {}", e.what()));
        return false;
    }

    auto tgt = registry.lookup(to);
    try {
        if (!tgt) {
            a.fd = src_fd;
            a.gen = src_gen;
            a.payload = build_chat_frame(
                aes_encrypt(src_key, std::string("Server")),
                aes_encrypt(src_key, std::string("No such user.")));
        } else {
            a.fd = tgt->fd;
            a.gen = tgt->gen;
            a.payload = build_chat_frame(
                aes_encrypt(*tgt->aeskey, from),
                aes_encrypt(*tgt->aeskey, msg));
        }
    } catch (const std::exception& e) {
        log_err(std::format("AES encrypt: {}", e.what()));
        return false;
    }
    return true;
}


void Reactor::emit_action(Action a) {
    auto it = conns.find(a.fd);
    if (it == conns.end() || it->second->gen != a.gen) return;
    enqueue_frame(*it->second, std::move(a.payload));
}


// ==================== 写事件 ====================
void Reactor::on_writable(Connection& c) {
    try_flush(c);
}


void Reactor::enqueue_frame(Connection& c, std::string frame) {
    c.outbuf.emplace_back(std::move(frame));
    try_flush(c);
}


void Reactor::try_flush(Connection& c) {
    while (!c.outbuf.empty()) {
        const std::string& front = c.outbuf.front();
        const char* p = front.data() + c.out_head_off;
        std::size_t rem = front.size() - c.out_head_off;

        ssize_t n = send(c.fd, p, rem, MSG_NOSIGNAL);
        if (n > 0) {
            c.out_head_off += n;
            if (c.out_head_off == front.size()) {
                c.outbuf.pop_front();
                c.out_head_off = 0;
            }
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        close_conn(c.fd, std::strerror(errno));
        return;
    }

    update_epoll_events(c);

    if (c.state == ConnState::CLOSING && c.outbuf.empty()) {
        close_conn(c.fd, "rejected");
    }
}


void Reactor::update_epoll_events(Connection& c) {
    bool want_out = !c.outbuf.empty();
    if (want_out == c.epollout_on) return;

    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLRDHUP | (want_out ? EPOLLOUT : 0u);
    ev.data.ptr = &c;
    epoll_ctl(epfd, EPOLL_CTL_MOD, c.fd, &ev);
    c.epollout_on = want_out;
}


// ==================== 关闭连接 ====================
void Reactor::close_conn(int fd, const char* reason) {
    auto it = conns.find(fd);
    if (it == conns.end()) return;

    auto conn = std::move(it->second);
    conns.erase(it);

    if (conn->state == ConnState::READY) {
        log_out(std::format("Client {} disconnected ({})", conn->username, reason ? reason : ""));
    } else if (conn->state != ConnState::CLOSING) {
        log_out(std::format("Connection {}:{} closed before handshake done ({})",
            conn->cli_ip, conn->cli_port, reason ? reason : ""));
    }

    if (!conn->username.empty() && conn->state != ConnState::CLOSING) {
        registry.erase_if_match(conn->username, conn->fd, conn->gen);
    }

    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
}


// ==================== 跨线程唤醒 ====================
void Reactor::post_action(Action a) {
    bool need_wake = false;
    {
        std::lock_guard lock(actions_mtx);
        need_wake = actions.empty();
        actions.emplace_back(std::move(a));
    }
    if (need_wake) {
        uint64_t one = 1;
        ssize_t w = write(wake_fd, &one, sizeof(one));
        (void)w;
    }
}


void Reactor::on_wakeup() {
    uint64_t dummy;
    while (read(wake_fd, &dummy, sizeof(dummy)) > 0) {}

    std::deque<Action> local;
    {
        std::lock_guard lock(actions_mtx);
        local.swap(actions);
    }

    for (auto& a : local) emit_action(std::move(a));
}


// ==================== 握手超时清理 ====================
void Reactor::sweep_timers() {
    int64_t now = now_ms();
    while (!timers.empty()) {
        const auto& top = timers.top();
        if (top.deadline_ms > now) break;

        int fd = top.fd;
        int gen = top.gen;
        timers.pop();

        auto it = conns.find(fd);
        if (it == conns.end() || it->second->gen != gen) continue;
        if (it->second->state == ConnState::READY) continue;

        log_out(std::format("Handshake timeout: {}:{}", it->second->cli_ip, it->second->cli_port));
        close_conn(fd, "handshake timeout");
    }
}
