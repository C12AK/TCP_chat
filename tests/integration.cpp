#include "e2e.h"
#include "net.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

static int g_fail = 0; // CHECK 失败次数。异常路径也会把它加一
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << "FAIL " << __LINE__ << " " << #cond << std::endl;          \
            ++g_fail;                                                               \
        }                                                                           \
    } while (0)

// 集成测试用的阻塞客户端。acc 留下还没切完的入站字节。
struct Client {
    int fd = -1;
    std::string acc;
    std::string priv;
    std::string pub;
    uint64_t id = 0;

    // 连到本机端口。失败返回 false。
    bool connect_to(int port) {
        fd = socket(PF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) return false;
        set_tcp_nodelay(fd);
        return true;
    }

    // 组帧并阻塞发送，直到全部写完。
    void send_frame(MsgType t, const std::string& payload) {
        std::string f = build_frame(t, payload);
        blocking_send_all(fd, f.data(), f.size());
    }

    // 阻塞直到切出一帧。对端关闭或出错则抛异常。
    Frame recv_frame() {
        Frame f;
        int rc = blocking_recv_frame(fd, acc, f);
        if (rc != 1) throw std::runtime_error("recv");
        return f;
    }

    // 丢掉前面类型不对的帧，直到等到 t。系统说明和会话列表常常插在期望的帧前面。
    Frame wait_type(MsgType t) {
        for (int i = 0; i < 8; ++i) {
            Frame f = recv_frame();
            if (f.type == t) return f;
        }
        throw std::runtime_error("missing frame");
    }

    // 关掉套接字并清掉半包，下一次 connect_to 从空缓冲开始。
    void close_fd() {
        if (fd >= 0) close(fd);
        fd = -1;
        acc.clear();
    }
};

// 试着连一次，用来等 fork 出来的 srv 开始 listen。
static bool port_open(int port) {
    int fd = socket(PF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    int rc = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    close(fd);
    return rc == 0;
}

// 集成测试。必须在仓库根目录运行：它会 fork ./srv。
// 覆盖注册、顶号、好友、收发、补拉、删好友、群、心跳超时。
int main() {
    const int port = 18081;
    const char* db = "/tmp/tcpchat_live/s.db";
    std::system("rm -rf /tmp/tcpchat_live logs && mkdir -p /tmp/tcpchat_live");
    pid_t pid = fork();
    if (pid == 0) {
        setenv("TCPCHAT_IDLE_MS", "2500", 1);
        setenv("TCPCHAT_QUEUE_CAP", "2", 1);
        execl("./srv", "srv", "18081", db, nullptr);
        _exit(1);
    }
    for (int i = 0; i < 50 && !port_open(port); ++i) usleep(50000);
    CHECK(port_open(port));

    try {
        Client a, b;
        CHECK(e2e_generate(a.priv, a.pub));
        CHECK(e2e_generate(b.priv, b.pub));
        CHECK(a.connect_to(port));
        W reg;
        reg.str("alice");
        reg.str("pw");
        reg.str(a.pub);
        a.send_frame(MsgType::Register, reg.take());
        Frame fr = a.recv_frame();
        CHECK(fr.type == MsgType::RegisterOk);
        R rr(fr.payload);
        CHECK(rr.u64(a.id) && a.id > 1);
        a.close_fd();

        CHECK(b.connect_to(port));
        W regb;
        regb.str("bob");
        regb.str("pw");
        regb.str(b.pub);
        b.send_frame(MsgType::Register, regb.take());
        fr = b.recv_frame();
        CHECK(fr.type == MsgType::RegisterOk);
        R rb(fr.payload);
        CHECK(rb.u64(b.id));
        b.close_fd();

        CHECK(a.connect_to(port));
        W login;
        login.str("alice");
        login.str("pw");
        a.send_frame(MsgType::Login, login.take());
        fr = a.recv_frame();
        CHECK(fr.type == MsgType::LoginOk);
        fr = a.recv_frame();
        CHECK(fr.type == MsgType::ConvList);

        Client a2;
        CHECK(a2.connect_to(port));
        W login2;
        login2.str("alice");
        login2.str("pw");
        a2.send_frame(MsgType::Login, login2.take());
        fr = a2.recv_frame();
        CHECK(fr.type == MsgType::LoginOk);
        fr = a.recv_frame();
        CHECK(fr.type == MsgType::Kick);
        a.close_fd();
        a.fd = a2.fd;   // 后面的步骤沿用新登录的这条连接
        a.acc = a2.acc;
        fr = a.recv_frame();
        CHECK(fr.type == MsgType::ConvList);

        CHECK(b.connect_to(port));
        W lb;
        lb.str("bob");
        lb.str("pw");
        b.send_frame(MsgType::Login, lb.take());
        CHECK(b.recv_frame().type == MsgType::LoginOk);
        CHECK(b.recv_frame().type == MsgType::ConvList);

        W search;
        search.str("bob");
        a.send_frame(MsgType::FriendSearch, search.take());
        fr = a.wait_type(MsgType::FriendSearchResult);
        R rs(fr.payload);
        uint8_t rel = 0;
        uint64_t found = 0;
        std::string name;
        CHECK(rs.u8(rel) && rel == 0);
        CHECK(rs.u64(found) && found == b.id);
        CHECK(rs.str(name) && name == "bob");

        W req;
        req.u64(b.id);
        a.send_frame(MsgType::FriendRequest, req.take());
        CHECK(a.wait_type(MsgType::FriendRequest).type == MsgType::FriendRequest);
        fr = b.wait_type(MsgType::Push);
        std::string text;
        {
            R rp(fr.payload);
            uint64_t id, conv, sender, ts;
            std::string blob;
            CHECK(rp.u64(id) && rp.u64(conv) && rp.u64(sender) && rp.u64(ts) && rp.str(blob));
            CHECK(e2e_open(b.priv, blob, text));
            CHECK(text.find("alice") != std::string::npos);
        }

        W acc;
        acc.u64(a.id);
        acc.u8(1);
        b.send_frame(MsgType::FriendRespond, acc.take());
        fr = a.wait_type(MsgType::ConvList);
        uint64_t direct = 0;
        std::string peer_pub;
        {
            R rc(fr.payload);
            uint64_t rev = 0, n = 0;
            CHECK(rc.u64(rev)); // 第一个 u64 是列表版本，后面才是会话个数
            CHECK(rc.u64(n));
            for (uint64_t i = 0; i < n; ++i) {
                uint64_t id, last, latest, owner, nm;
                uint8_t kind = 0;
                std::string title;
                CHECK(rc.u64(id) && rc.u8(kind) && rc.str(title));
                CHECK(rc.u64(last) && rc.u64(latest) && rc.u64(owner) && rc.u64(nm));
                if (kind == 0) direct = id;
                for (uint64_t k = 0; k < nm; ++k) {
                    uint64_t uid;
                    std::string un, up;
                    CHECK(rc.u64(uid) && rc.str(un) && rc.str(up));
                    if (uid == b.id) peer_pub = up;
                }
            }
        }
        CHECK(direct != 0);
        CHECK(peer_pub == b.pub);
        b.wait_type(MsgType::ConvList);

        std::string to_b, to_a;
        CHECK(e2e_seal(b.pub, "hello-bob", to_b));
        CHECK(e2e_seal(a.pub, "hello-bob", to_a));
        W chat;
        chat.u64(direct);
        chat.u64(99);
        chat.u64(2);
        chat.u64(b.id);
        chat.str(to_b);
        chat.u64(a.id);
        chat.str(to_a);
        a.send_frame(MsgType::ChatSend, chat.take());
        fr = a.wait_type(MsgType::ChatAck);
        R rack(fr.payload);
        uint64_t nonce = 0, msg = 0;
        CHECK(rack.u64(nonce) && nonce == 99);
        CHECK(rack.u64(msg) && msg > 0);
        fr = b.wait_type(MsgType::Push);
        {
            R rp(fr.payload);
            uint64_t id, conv, sender, ts;
            std::string blob, plain;
            CHECK(rp.u64(id) && rp.u64(conv) && rp.u64(sender) && rp.u64(ts) && rp.str(blob));
            CHECK(e2e_open(b.priv, blob, plain));
            CHECK(plain == "hello-bob");
            W rec;
            rec.u8(2);
            rec.u64(conv);
            rec.u64(id);
            b.send_frame(MsgType::Receipt, rec.take());
        }
        fr = a.wait_type(MsgType::Receipt);
        {
            R rp(fr.payload);
            uint8_t kind = 0;
            uint64_t conv, id, from;
            CHECK(rp.u8(kind) && kind == 2);
            CHECK(rp.u64(conv) && rp.u64(id) && rp.u64(from) && from == b.id);
        }

        b.close_fd();
        CHECK(b.connect_to(port));
        W lb2;
        lb2.str("bob");
        lb2.str("pw");
        b.send_frame(MsgType::Login, lb2.take());
        CHECK(b.recv_frame().type == MsgType::LoginOk);
        CHECK(b.recv_frame().type == MsgType::ConvList);
        W sync;
        sync.u64(0);
        b.send_frame(MsgType::SyncReq, sync.take());
        fr = b.wait_type(MsgType::SyncBatch);
        {
            R rs2(fr.payload);
            uint64_t n = 0;
            CHECK(rs2.u64(n) && n >= 1);
            bool saw = false;
            for (uint64_t i = 0; i < n; ++i) {
                uint64_t id, conv, sender, ts;
                std::string blob, plain;
                CHECK(rs2.u64(id) && rs2.u64(conv) && rs2.u64(sender) && rs2.u64(ts) && rs2.str(blob));
                if (e2e_open(b.priv, blob, plain) && plain == "hello-bob") saw = true;
            }
            CHECK(saw);
        }

        W del;
        del.u64(b.id);
        a.send_frame(MsgType::FriendDelete, del.take());
        fr = a.wait_type(MsgType::Push);
        {
            R rp(fr.payload);
            uint64_t id, conv, sender, ts;
            std::string blob, plain;
            CHECK(rp.u64(id) && rp.u64(conv) && rp.u64(sender) && rp.u64(ts) && rp.str(blob));
            CHECK(e2e_open(a.priv, blob, plain));
            CHECK(plain.find("解除好友") != std::string::npos);
        }

        W g;
        g.str("小组");
        a.send_frame(MsgType::GroupCreate, g.take());
        fr = a.wait_type(MsgType::GroupCreateOk);
        uint64_t gid = 0;
        {
            R rok(fr.payload);
            std::string title;
            CHECK(rok.u64(gid) && gid != 0);
            CHECK(rok.str(title) && title == "小组");
        }

        // 已经不是好友，邀请应当被拒绝。
        W inv;
        inv.u64(gid);
        inv.u64(b.id);
        a.send_frame(MsgType::GroupInvite, inv.take());
        fr = a.wait_type(MsgType::Error);
        CHECK(fr.type == MsgType::Error);

        W req2;
        req2.u64(b.id);
        a.send_frame(MsgType::FriendRequest, req2.take());
        a.wait_type(MsgType::FriendRequest);
        b.wait_type(MsgType::Push);
        W acc2;
        acc2.u64(a.id);
        acc2.u8(1);
        b.send_frame(MsgType::FriendRespond, acc2.take());
        a.wait_type(MsgType::ConvList);
        b.wait_type(MsgType::ConvList);
        W inv2;
        inv2.u64(gid);
        inv2.u64(b.id);
        a.send_frame(MsgType::GroupInvite, inv2.take());
        CHECK(b.wait_type(MsgType::Push).type == MsgType::Push);
        CHECK(b.wait_type(MsgType::ConvList).type == MsgType::ConvList);

        W dis;
        dis.u64(gid);
        a.send_frame(MsgType::GroupDissolve, dis.take());
        CHECK(a.wait_type(MsgType::Push).type == MsgType::Push);

        // 心跳超时
        Client idle;
        CHECK(idle.connect_to(port));
        std::this_thread::sleep_for(std::chrono::milliseconds(3500));
        Frame junk;
        int rc = blocking_recv_frame(idle.fd, idle.acc, junk);
        CHECK(rc <= 0);
        idle.close_fd();

        a.close_fd();
        b.close_fd();
    } catch (const std::exception& e) {
        std::cerr << "EXC " << e.what() << std::endl;
        ++g_fail;
    }

    kill(pid, SIGTERM);
    waitpid(pid, nullptr, 0);
    if (g_fail) {
        std::cerr << g_fail << " live checks failed" << std::endl;
        return 1;
    }
    std::cout << "live ok" << std::endl;
    return 0;
}
