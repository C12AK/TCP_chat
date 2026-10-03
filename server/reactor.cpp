#include "reactor.h"

#include "connection.h"
#include "e2e.h"
#include "log.h"
#include "net.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <netinet/in.h>
#include <openssl/rand.h>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

constexpr int kMaxEvents = 1024;
constexpr int kReadChunk = 4096;

// 这两个对象的地址只当作 epoll 标记。data.ptr 等于它们时，表示监听套接字或 eventfd，不是某个 Connection。
char listen_tag;
char wake_tag;

// 单调时钟的毫秒数，用来量连接闲了多久。改系统时间不会让它往回跳。
int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// 系统说明用的 nonce。随机失败时退回当前毫秒，只求不要和上一句轻易撞上。
uint64_t rand_u64() {
    uint64_t v = 0;
    if (RAND_bytes(reinterpret_cast<unsigned char*>(&v), sizeof(v)) != 1) {
        v = static_cast<uint64_t>(now_ms());
    }
    return v;
}

// 组一帧 Error，载荷是一句原因。
std::string err_frame(const std::string& s) {
    W w;
    w.str(s);
    return build_frame(MsgType::Error, w.take());
}

}  // namespace

// 建立监听套接字、epoll 和用来叫醒自己的 eventfd，并启动线程池。
Reactor::Reactor(int port, Database& db, std::size_t workers, int idle_ms, int queue_cap)
    : port(port), db(db), idle_ms(idle_ms), queue_cap(queue_cap), pool(workers) {
    listen_sock = make_listen_socket(port, 4096);
    if (listen_sock < 0) throw std::runtime_error(std::format("listen: {}", std::strerror(errno)));
    set_nonblocking(listen_sock);

    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) throw std::runtime_error("epoll_create1");
    wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd < 0) throw std::runtime_error("eventfd");

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.ptr = &listen_tag;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_sock, &ev);
    ev.data.ptr = &wake_tag;
    epoll_ctl(epfd, EPOLL_CTL_ADD, wake_fd, &ev);
}

// 关掉 eventfd、监听套接字、还在表里的连接和 epoll。线程池随后会把剩余任务跑完。
Reactor::~Reactor() {
    if (wake_fd >= 0) close(wake_fd);
    if (listen_sock >= 0) close(listen_sock);
    for (auto& [fd, _] : conns) close(fd);
    if (epfd >= 0) close(epfd);
}

// 事件循环。监听套接字、eventfd、普通连接分三条路。每一轮结束扫一次空闲连接。
void Reactor::run() {
    log_info(std::format("服务器已在端口 {} 启动", port));
    std::vector<epoll_event> events(kMaxEvents);
    while (true) {
        int nfds = epoll_wait(epfd, events.data(), kMaxEvents, 1000);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            log_error(std::format("epoll_wait: {}", std::strerror(errno)));
            break;
        }
        for (int i = 0; i < nfds; ++i) {
            void* p = events[i].data.ptr;
            uint32_t evs = events[i].events;
            if (p == &listen_tag) { on_accept(); continue; }
            if (p == &wake_tag) { on_wakeup(); continue; }
            auto* c = static_cast<Connection*>(p);
            int fd = c->fd;
            if (evs & (EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP)) on_readable(*c, evs);
            if (evs & EPOLLOUT) {
                auto it = conns.find(fd);
                if (it != conns.end()) on_writable(*it->second);
            }
        }
        sweep_idle();
    }
}

// 把积压的新连接全部接受下来。新连接是 Fresh，还不能发聊天。
void Reactor::on_accept() {
    while (true) {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        int fd = accept4(listen_sock, reinterpret_cast<sockaddr*>(&addr), &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            log_error(std::format("accept: {}", std::strerror(errno)));
            return;
        }
        set_tcp_nodelay(fd);
        auto conn = std::make_unique<Connection>();
        conn->fd = fd;
        conn->gen = next_gen++; // 只增。fd 被内核复用后，旧任务靠这个数字对不上就会被丢掉
        conn->last_rx_ms = now_ms();
        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        conn->cli_ip = ip;
        conn->cli_port = ntohs(addr.sin_port);

        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLRDHUP;
        ev.data.ptr = conn.get();
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            close(fd);
            continue;
        }
        conns.emplace(fd, std::move(conn));
    }
}

// 读到 EAGAIN 为止，切出完整帧后交给 handle_frame。对端关闭或协议非法则拆连接。
void Reactor::on_readable(Connection& c, uint32_t evs) {
    int fd = c.fd;
    c.last_rx_ms = now_ms(); // 心跳和业务字节都算「还活着」
    bool peer_closed = false; // recv 返回 0：对端关掉了写方向
    char buf[kReadChunk];
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

    try {
        while (true) {
            auto f = parse_frame(c.inbuf, c.in_off);
            if (!f) break;
            handle_frame(c, f->type, f->payload);
            if (!conns.count(fd)) return;
        }
        compact_buf(c.inbuf, c.in_off);
    } catch (const std::exception& e) {
        log_error(std::format("连接 {}:{} 协议错误: {}", c.cli_ip, c.cli_port, e.what()));
        close_conn(fd, "protocol");
        return;
    }

    if (peer_closed) close_conn(fd, "EOF");
    else if (evs & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) close_conn(fd, "hangup");
}

// 未登录只处理注册和登录，口令哈希在线程池里算，结果用 eventfd 送回本线程。
// 已登录的请求先占该用户的队列名额，再进线程池写库；名额用尽则直接回 QueueBusy。
void Reactor::handle_frame(Connection& c, MsgType type, const std::string& payload) {
    if (type == MsgType::Heartbeat) return;

    if (c.state != ConnState::Ready && type != MsgType::Register && type != MsgType::Login) {
        enqueue_frame(c, err_frame("请先登录"));
        return;
    }

    // 注册、登录不占聊天队列。工人算完口令后投回 LoginOk / RegisterOk / 失败帧
    if (type == MsgType::Register || type == MsgType::Login) {
        std::string user, pass, pub;
        R r(payload);
        if (!r.str(user) || !r.str(pass) || (type == MsgType::Register && !r.str(pub))) {
            enqueue_frame(c, err_frame("请求格式不对"));
            return;
        }
        int fd = c.fd;
        int gen = c.gen;
        bool is_reg = type == MsgType::Register;
        pool.enqueue([this, fd, gen, user, pass, pub, is_reg] {
            Action a;
            a.fd = fd;
            a.gen = gen;
            if (is_reg) {
                uint64_t id = 0;
                std::string err;
                if (!db.register_user(user, pass, pub, id, err)) {
                    a.frame = err_frame(err);
                } else {
                    a.kind = 2;
                    a.user_id = id;
                    W w;
                    w.u64(id);
                    a.frame = build_frame(MsgType::RegisterOk, w.take());
                    log_info(std::format("注册成功 id={} name={}", id, user));
                }
            } else {
                Database::User u;
                std::string err;
                if (!db.verify_login(user, pass, u, err)) {
                    W w;
                    w.str(err);
                    a.frame = build_frame(MsgType::LoginFail, w.take());
                    log_info(std::format("登录失败 name={}", user));
                } else {
                    a.kind = 1;
                    a.user_id = u.id;
                    a.username = u.username;
                    W w;
                    w.u64(u.id);
                    a.frame = build_frame(MsgType::LoginOk, w.take());
                }
            }
            post_action(std::move(a));
        });
        return;
    }

    if (!try_acquire(c.user_id)) {
        W w;
        w.str("发送太快，请稍后再试");
        enqueue_frame(c, build_frame(MsgType::QueueBusy, w.take()));
        return;
    }

    uint64_t uid = c.user_id;
    std::string username = c.username;
    int fd = c.fd;
    int gen = c.gen;
    pool.enqueue([this, type, payload, uid, username, fd, gen] {
        // 下面任何一条 return 离开时，都把这个用户占用的写库名额还回去
        struct Guard {
            Reactor* self;
            uint64_t uid;
            ~Guard() { self->release_user(uid); }
        } guard{this, uid};

        auto fail = [&](const std::string& s) { send_frame_to(fd, gen, MsgType::Error, [&] {
            W w; w.str(s); return w.take();
        }()); };

        // 补拉：只下发收件人是自己的密文。满 100 条时客户端用最后的 id 再要一次
        if (type == MsgType::SyncReq) {
            R r(payload);
            uint64_t after = 0;
            if (!r.u64(after)) { fail("请求格式不对"); return; }
            auto rows = db.sync_for(uid, after, 100);
            W w;
            w.u64(rows.size());
            for (const auto& m : rows) {
                w.u64(m.msg_id);
                w.u64(m.conv_id);
                w.u64(m.sender_id);
                w.u64(m.ts_ms);
                w.str(m.ciphertext);
            }
            send_frame_to(fd, gen, MsgType::SyncBatch, w.take());
            return;
        }

        // 发信：会话成员必须每人一份密文。先落库再给发送者 ChatAck，在线成员收 Push
        if (type == MsgType::ChatSend) {
            R r(payload);
            uint64_t conv = 0, nonce = 0, n = 0;
            if (!r.u64(conv) || !r.u64(nonce) || !r.u64(n) || n == 0 || n > 50) {
                fail("消息格式不对");
                return;
            }
            std::vector<Database::MemberSeal> copies;
            for (uint64_t i = 0; i < n; ++i) {
                Database::MemberSeal s;
                if (!r.u64(s.user_id) || !r.str(s.ciphertext)) { fail("消息格式不对"); return; }
                copies.push_back(std::move(s));
            }
            int kind = 0;
            uint64_t owner = 0;
            if (!db.conv_kind(conv, kind, owner) || kind == 2 || !db.is_member(conv, uid)) {
                fail("不能往这个会话发消息");
                return;
            }
            auto mem = db.members(conv);
            if (copies.size() != mem.size()) { fail("请为每位成员各封一份"); return; }
            for (const auto& s : copies) {
                if (!db.is_member(conv, s.user_id)) { fail("收件人不是会话成员"); return; }
            }
            uint64_t msg_id = 0;
            bool dup = false;
            std::string err;
            if (!db.insert_message(conv, uid, nonce, static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count()),
                    copies, msg_id, dup, err)) {
                fail(err);
                return;
            }
            W ack;
            ack.u64(nonce);
            ack.u64(msg_id);
            send_frame_to(fd, gen, MsgType::ChatAck, ack.take());
            if (!dup) { // 重复提交只回已有编号，不再给成员推一次
                for (const auto& s : copies) {
                    W p;
                    p.u64(msg_id);
                    p.u64(conv);
                    p.u64(uid);
                    p.u64(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count()));
                    p.str(s.ciphertext);
                    send_to_user(s.user_id, build_frame(MsgType::Push, p.take()));
                }
            }
            return;
        }

        // 回执：已读记下游标；两种回执都转给原发送者（对方不在线就只记已读）
        if (type == MsgType::Receipt) {
            R r(payload);
            uint8_t kind = 0;
            uint64_t conv = 0, msg = 0;
            if (!r.u8(kind) || !r.u64(conv) || !r.u64(msg)) { fail("回执格式不对"); return; }
            if (kind == 2) db.set_read(uid, conv, msg);
            uint64_t conv2 = 0, sender = 0;
            if (!db.message_info(msg, conv2, sender)) return;
            W w;
            w.u8(kind);
            w.u64(conv2);
            w.u64(msg);
            w.u64(uid);
            send_to_user(sender, build_frame(MsgType::Receipt, w.take()));
            return;
        }

        // 按完整用户名查关系，不在这里改数据
        if (type == MsgType::FriendSearch) {
            R r(payload);
            std::string name;
            if (!r.str(name)) { fail("请求格式不对"); return; }
            Database::User u;
            int rel = 255;
            W w;
            if (!db.search_user(uid, name, u, rel) || u.id == Database::kSystemId) {
                w.u8(255);
                w.u64(0);
                w.str("");
            } else {
                w.u8(static_cast<uint8_t>(rel));
                w.u64(u.id);
                w.str(u.username);
            }
            send_frame_to(fd, gen, MsgType::FriendSearchResult, w.take());
            return;
        }

        // 写入申请，并给对方的系统会话推一条说明
        if (type == MsgType::FriendRequest) {
            R r(payload);
            uint64_t to = 0;
            if (!r.u64(to)) { fail("请求格式不对"); return; }
            std::string err;
            if (!db.add_request(uid, to, err)) { fail(err); return; }
            Database::User me;
            db.user_by_id(uid, me);
            notify_system(to, me.username + " 请求添加你为好友");
            send_frame_to(fd, gen, MsgType::FriendRequest, std::string());
            return;
        }

        // 同意则建立私聊会话。双方若在线，各收一份新的会话列表
        if (type == MsgType::FriendRespond) {
            R r(payload);
            uint64_t requester = 0;
            uint8_t accept = 0;
            if (!r.u64(requester) || !r.u8(accept)) { fail("请求格式不对"); return; }
            uint64_t conv = 0;
            std::string err;
            if (!db.respond_request(uid, requester, accept == 1, conv, err)) { fail(err); return; }
            Database::User me, other;
            db.user_by_id(uid, me);
            db.user_by_id(requester, other);
            if (accept == 1) {
                notify_system(uid, "你已添加 " + other.username + " 为好友");
                notify_system(requester, me.username + " 已同意你的好友申请");
            } else {
                notify_system(requester, me.username + " 拒绝了你的好友申请");
            }
            send_to_user(uid, conv_list_frame(uid));
            send_to_user(requester, conv_list_frame(requester));
            return;
        }

        // 两边都从好友和该私聊里移除，系统会话各留一句说明
        if (type == MsgType::FriendDelete) {
            R r(payload);
            uint64_t other = 0;
            if (!r.u64(other)) { fail("请求格式不对"); return; }
            uint64_t conv = 0;
            if (!db.delete_friend(uid, other, conv)) { fail("不是好友"); return; }
            Database::User me, peer;
            db.user_by_id(uid, me);
            db.user_by_id(other, peer);
            notify_system(uid, "你已与 " + peer.username + " 解除好友");
            notify_system(other, me.username + " 已与你解除好友");
            send_to_user(uid, conv_list_frame(uid));
            send_to_user(other, conv_list_frame(other));
            return;
        }

        // 群主创建时自己就是唯一成员。邀请只允许拉自己的好友，上限 50 人
        if (type == MsgType::GroupCreate) {
            R r(payload);
            std::string title;
            if (!r.str(title)) { fail("请求格式不对"); return; }
            uint64_t conv = 0;
            std::string err;
            if (!db.create_group(uid, title, conv, err)) { fail(err); return; }
            W w;
            w.u64(conv);
            w.str(title);
            send_frame_to(fd, gen, MsgType::GroupCreateOk, w.take());
            send_to_user(uid, conv_list_frame(uid));
            return;
        }

        if (type == MsgType::GroupInvite) {
            R r(payload);
            uint64_t conv = 0, who = 0;
            if (!r.u64(conv) || !r.u64(who)) { fail("请求格式不对"); return; }
            std::string err;
            if (!db.invite_group(conv, uid, who, err)) { fail(err); return; }
            int kind = 0;
            uint64_t owner = 0;
            db.conv_kind(conv, kind, owner);
            Database::Conv listed;
            std::string title;
            for (const auto& c : db.list_convs(uid)) if (c.id == conv) title = c.title;
            Database::User me;
            db.user_by_id(uid, me);
            notify_system(who, me.username + " 邀请你加入群 " + title);
            send_to_user(who, conv_list_frame(who));
            send_to_user(uid, conv_list_frame(uid));
            return;
        }

        if (type == MsgType::GroupLeave) {
            R r(payload);
            uint64_t conv = 0;
            if (!r.u64(conv)) { fail("请求格式不对"); return; }
            std::string err;
            if (!db.leave_group(conv, uid, err)) { fail(err); return; }
            send_to_user(uid, conv_list_frame(uid));
            return;
        }

        if (type == MsgType::GroupDissolve) {
            R r(payload);
            uint64_t conv = 0;
            if (!r.u64(conv)) { fail("请求格式不对"); return; }
            std::string title;
            for (const auto& c : db.list_convs(uid)) if (c.id == conv) title = c.title;
            std::vector<uint64_t> former;
            std::string err;
            if (!db.dissolve_group(conv, uid, former, err)) { fail(err); return; }
            for (uint64_t id : former) {
                if (id == Database::kSystemId) continue;
                notify_system(id, "群 " + title + " 已解散");
                send_to_user(id, conv_list_frame(id));
            }
            return;
        }

        fail("不认识的请求");
    });
}

// 组装带版本号的会话列表。版本只增，客户端丢掉不比本地新的那一份。
std::string Reactor::conv_list_frame(uint64_t user_id) {
    uint64_t rev = 0;
    {
        std::lock_guard lock(rev_mu);
        rev = ++list_rev[user_id];
    }
    W w;
    w.u64(rev);
    auto convs = db.list_convs(user_id);
    w.u64(convs.size());
    for (const auto& c : convs) {
        w.u64(c.id);
        w.u8(static_cast<uint8_t>(c.kind));
        w.str(c.title);
        w.u64(c.last_read_id);
        w.u64(c.latest_id);
        w.u64(c.owner_id);
        auto mem = db.members(c.id);
        w.u64(mem.size());
        for (uint64_t id : mem) {
            Database::User u;
            db.user_by_id(id, u);
            w.u64(id);
            w.str(u.username);
            w.str(u.pubkey);
        }
    }
    return build_frame(MsgType::ConvList, w.take());
}

// 系统提示：服务器知道原文，用收件人长期公钥封上再写入系统会话。对方不在线则只留在库里
void Reactor::notify_system(uint64_t user_id, const std::string& text) {
    Database::User u;
    if (!db.user_by_id(user_id, u) || u.pubkey.size() != 32) return;
    std::string blob;
    if (!e2e_seal(u.pubkey, text, blob)) return;
    uint64_t conv = db.system_conv(user_id);
    uint64_t msg_id = 0;
    bool dup = false;
    std::string err;
    std::vector<Database::MemberSeal> copies{{user_id, blob}};
    uint64_t ts = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    if (!db.insert_message(conv, Database::kSystemId, rand_u64(), ts, copies, msg_id, dup, err)) return;
    W p;
    p.u64(msg_id);
    p.u64(conv);
    p.u64(Database::kSystemId);
    p.u64(ts);
    p.str(blob);
    send_to_user(user_id, build_frame(MsgType::Push, p.take()));
}

// 登录成功落在 Reactor 线程：先把旧连接标记为发完就关，再占住在线表，最后下发会话列表
void Reactor::finish_login(Connection& c, uint64_t user_id, const std::string& username) {
    auto it = online.find(user_id);
    // fd 和 gen 都相同说明就是自己，不用踢。对不上才是同一账号的另一条连接
    if (it != online.end() && !(it->second.fd == c.fd && it->second.gen == c.gen)) {
        auto old = conns.find(it->second.fd);
        if (old != conns.end() && old->second->gen == it->second.gen) {
            W w;
            w.str("账号在别处登录");
            enqueue_frame(*old->second, build_frame(MsgType::Kick, w.take()));
            old->second->state = ConnState::Closing;
        }
    }
    c.state = ConnState::Ready;
    c.user_id = user_id;
    c.username = username;
    {
        std::lock_guard lock(online_mu);
        online[user_id] = Online{c.fd, c.gen};
    }
    W w;
    w.u64(user_id);
    enqueue_frame(c, build_frame(MsgType::LoginOk, w.take()));
    enqueue_frame(c, conv_list_frame(user_id));
    log_info(std::format("登录成功 id={} name={} from {}:{}", user_id, username, c.cli_ip, c.cli_port));
}

// 放进出站队列并立刻尝试写。写不完的留给 EPOLLOUT。
void Reactor::enqueue_frame(Connection& c, std::string frame) {
    c.outbuf.emplace_back(std::move(frame));
    try_flush(c);
}

// 非阻塞发送。内核缓冲满就停下；状态为 Closing 且队列已空才真正关闭，好让 Kick 先出去。
void Reactor::try_flush(Connection& c) {
    while (!c.outbuf.empty()) {
        const std::string& front = c.outbuf.front();
        ssize_t n = send(c.fd, front.data() + c.out_head_off, front.size() - c.out_head_off, MSG_NOSIGNAL);
        if (n > 0) {
            c.out_head_off += static_cast<std::size_t>(n);
            if (c.out_head_off == front.size()) {
                c.outbuf.pop_front();
                c.out_head_off = 0;
            }
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break; // 内核发送缓冲满了，等可写事件
        close_conn(c.fd, std::strerror(errno));
        return;
    }
    update_epoll_events(c);
    if (c.state == ConnState::Closing && c.outbuf.empty()) close_conn(c.fd, "closing");
}

// 出站队列非空才监听 EPOLLOUT。空闲连接若一直监听可写，会被反复叫醒。
void Reactor::update_epoll_events(Connection& c) {
    bool want = !c.outbuf.empty();
    if (want == c.epollout_on) return;
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLRDHUP | (want ? EPOLLOUT : 0u);
    ev.data.ptr = &c;
    epoll_ctl(epfd, EPOLL_CTL_MOD, c.fd, &ev);
    c.epollout_on = want;
}

// 内核发送缓冲有空位，把上次没写完的字节继续送出去。
void Reactor::on_writable(Connection& c) { try_flush(c); }

// 从 epoll 和连接表摘掉。在线表里的 fd 和 gen 仍是这一条时，才把该用户标成离线。
void Reactor::close_conn(int fd, const char* reason) {
    auto it = conns.find(fd);
    if (it == conns.end()) return;
    auto conn = std::move(it->second);
    conns.erase(it);
    if (conn->user_id != 0) {
        std::lock_guard lock(online_mu);
        auto o = online.find(conn->user_id);
        if (o != online.end() && o->second.fd == fd && o->second.gen == conn->gen) online.erase(o); // gen 不同说明这个用户已经换到新连接
    }
    if (conn->state == ConnState::Ready) {
        log_info(std::format("用户 {} 断开 ({})", conn->username, reason ? reason : ""));
    }
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
}

// 该用户未写完的任务数加一。已达上限返回 false，调用方回 QueueBusy，这条不入库。
bool Reactor::try_acquire(uint64_t user_id) {
    std::lock_guard lock(queue_mu);
    int& n = inflight[user_id];
    if (n >= queue_cap) return false;
    ++n;
    return true;
}

// 任务结束，名额减一。减到 0 就从表里删掉，避免空闲用户把表撑大。
void Reactor::release_user(uint64_t user_id) {
    std::lock_guard lock(queue_mu);
    auto it = inflight.find(user_id);
    if (it == inflight.end()) return;
    if (--it->second <= 0) inflight.erase(it);
}

// 任意线程把结果放进队列。只有队列从空变成非空时才写 eventfd，避免重复叫醒。
void Reactor::post_action(Action a) {
    bool wake = false; // 放进之前队列是空的，Reactor 可能正睡在 epoll_wait 里
    {
        std::lock_guard lock(actions_mu);
        wake = actions.empty();
        actions.push_back(std::move(a));
    }
    if (wake) {
        uint64_t one = 1;
        ssize_t w = write(wake_fd, &one, sizeof(one));
        (void)w;
    }
}

// 工人给某个连接发帧的入口。这里只投递，真正的 send 在 Reactor 线程。
void Reactor::send_frame_to(int fd, int gen, MsgType type, const std::string& payload) {
    Action a;
    a.fd = fd;
    a.gen = gen;
    a.frame = build_frame(type, payload);
    post_action(std::move(a));
}

// 按用户 id 找在线连接并投递。不在线就丢掉这帧，密文已经在库里，等对方来拉。
void Reactor::send_to_user(uint64_t user_id, const std::string& frame) {
    std::lock_guard lock(online_mu);
    auto it = online.find(user_id);
    if (it == online.end()) return;
    Action a;
    a.fd = it->second.fd;
    a.gen = it->second.gen;
    a.frame = frame;
    // 不能在持有 online_mu 时再锁 actions_mu 以外的东西；post_action 只锁 actions
    // 但这里已持有 online_mu。post_action 不拿 online_mu，顺序固定为 online -> actions，可接受。
    actions_mu.lock();
    bool wake = actions.empty();
    actions.push_back(std::move(a));
    actions_mu.unlock();
    if (wake) {
        uint64_t one = 1;
        ssize_t w = write(wake_fd, &one, sizeof(one));
        (void)w;
    }
}

// eventfd 可读。读空计数器，取出全部待发送结果。fd 已关或 gen 对不上的丢掉。
void Reactor::on_wakeup() {
    uint64_t dummy = 0; // 读出的是计数器里积压了多少次叫醒，值本身不用
    while (read(wake_fd, &dummy, sizeof(dummy)) > 0) {}
    std::deque<Action> local;
    {
        std::lock_guard lock(actions_mu);
        local.swap(actions);
    }
    for (auto& a : local) {
        auto it = conns.find(a.fd);
        if (it == conns.end() || it->second->gen != a.gen) continue; // 这个 fd 已经换成后来的连接
        if (a.kind == 1) {
            finish_login(*it->second, a.user_id, a.username);
            continue; // 登录成功不发工人组的那一帧，LoginOk 和会话列表由 finish_login 排队
        }
        enqueue_frame(*it->second, std::move(a.frame));
    }
}

// 超过 idle_ms 没有任何入站字节就断开。先收集 fd 再关，避免边遍历连接表边删除。
void Reactor::sweep_idle() {
    int64_t now = now_ms();
    std::vector<int> drop;
    for (auto& [fd, c] : conns) {
        if (now - c->last_rx_ms > idle_ms) drop.push_back(fd);
    }
    for (int fd : drop) {
        log_info(std::format("连接 {} 心跳超时", fd));
        close_conn(fd, "idle");
    }
}
