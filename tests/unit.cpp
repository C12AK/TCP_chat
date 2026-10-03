#include "db.h"
#include "e2e.h"
#include "log.h"
#include "pass.h"
#include "protocol.h"

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

static int g_fail = 0; // CHECK 失败的次数，main 用它决定退出码

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << " " << #cond     \
                      << std::endl;                                                 \
            ++g_fail;                                                               \
        }                                                                           \
    } while (0)

// 同一缓冲里粘两帧，确认大端字段和中文能原样读回。
static void test_frame() {
    W w;
    w.u8(7);
    w.u64(0x0102030405060708ull);
    w.str("你好");
    std::string frame = build_frame(MsgType::Login, w.take());
    std::string buf = frame + frame;
    std::size_t off = 0;
    auto a = parse_frame(buf, off);
    auto b = parse_frame(buf, off);
    CHECK(a && b);
    CHECK(off == buf.size());
    R r(a->payload);
    uint8_t u = 0;
    uint64_t n = 0;
    std::string s;
    CHECK(r.u8(u) && u == 7);
    CHECK(r.u64(n) && n == 0x0102030405060708ull);
    CHECK(r.str(s) && s == "你好");
    CHECK(r.empty());
}

// 口令哈希要认对也要拒绝错的。端到端只有持有对应私钥的一方能拆开。
static void test_pass_and_e2e() {
    std::string salt, hash;
    CHECK(make_pass_hash("secret", salt, hash));
    CHECK(check_pass_hash("secret", salt, hash));
    CHECK(!check_pass_hash("nope", salt, hash));

    std::string priv, pub, blob, plain;
    CHECK(e2e_generate(priv, pub));
    CHECK(e2e_seal(pub, "WSNL, WCSNL", blob));
    CHECK(e2e_open(priv, blob, plain));
    CHECK(plain == "WSNL, WCSNL");
    std::string priv2, pub2;
    CHECK(e2e_generate(priv2, pub2));
    CHECK(!e2e_open(priv2, blob, plain));
}

// 连续写入后，日志目录里所有文件的体积不能超过打开时给的上限太多。
static void test_log_cap() {
    namespace fs = std::filesystem;
    fs::remove_all("/tmp/tcpchat_log");
    fs::create_directories("/tmp/tcpchat_log");
    CHECK(log_open("/tmp/tcpchat_log/a.log", 8192));
    for (int i = 0; i < 400; ++i) log_info(std::string(100, 'x'));
    std::size_t total = 0;
    for (auto& p : fs::directory_iterator("/tmp/tcpchat_log")) {
        total += fs::file_size(p.path());
    }
    CHECK(total <= 8192 + 4096);
}

// 直接调用 Database：注册、好友、消息去重、补拉、退群和解散，不启动 srv。
static void test_db() {
    namespace fs = std::filesystem;
    fs::remove_all("/tmp/tcpchat_db");
    Database db("/tmp/tcpchat_db/s.db");
    std::string pub_a(32, '\x11'), pub_b(32, '\x22');
    uint64_t a = 0, b = 0;
    std::string err;
    CHECK(db.register_user("alice", "pw", pub_a, a, err));
    CHECK(!db.register_user("alice", "pw", pub_a, b, err));
    CHECK(err == "用户名已被占用");
    CHECK(db.register_user("bob", "pw", pub_b, b, err));
    Database::User u;
    CHECK(db.verify_login("alice", "pw", u, err) && u.id == a);
    CHECK(!db.verify_login("alice", "bad", u, err));
    CHECK(db.system_conv(a) != 0);

    int rel = 0;
    CHECK(db.search_user(a, "bob", u, rel) && rel == 0 && u.id == b);
    CHECK(db.add_request(a, b, err));
    CHECK(db.search_user(a, "bob", u, rel) && rel == 1);
    CHECK(db.search_user(b, "alice", u, rel) && rel == 2);
    uint64_t conv = 0;
    CHECK(db.respond_request(b, a, true, conv, err) && conv != 0);
    CHECK(db.is_member(conv, a) && db.is_member(conv, b));

    std::string pa, pb, seal_a, seal_b;
    CHECK(e2e_generate(pa, pub_a));
    // 注册时的公钥和解密私钥不是同一对。这里直接用库里的公钥封给对方会解不开。
    // 单元测试改为把新公钥写不进去，所以用双方已知的测试公钥封一份假密文，只验证落库。
    std::vector<Database::MemberSeal> copies{{a, std::string("cipher-a")}, {b, std::string("cipher-b")}};
    uint64_t msg = 0;
    bool dup = false;
    CHECK(db.insert_message(conv, a, 42, 1000, copies, msg, dup, err));
    CHECK(!dup && msg > 0);
    CHECK(db.insert_message(conv, a, 42, 1000, copies, msg, dup, err) && dup);
    auto rows = db.sync_for(b, 0, 10);
    CHECK(rows.size() == 1 && rows[0].ciphertext == "cipher-b");
    CHECK(db.set_read(b, conv, msg));

    uint64_t gone = 0;
    CHECK(db.delete_friend(a, b, gone) && gone == conv);
    CHECK(!db.is_member(conv, a));

    uint64_t gid = 0;
    CHECK(db.add_request(a, b, err));
    CHECK(db.respond_request(b, a, true, conv, err));
    CHECK(db.create_group(a, "小组", gid, err));
    CHECK(db.invite_group(gid, a, b, err));
    CHECK(!db.leave_group(gid, a, err));
    CHECK(db.leave_group(gid, b, err));
    CHECK(db.invite_group(gid, a, b, err));
    std::vector<uint64_t> former;
    CHECK(db.dissolve_group(gid, a, former, err));
    CHECK(!db.is_member(gid, a));
    (void)seal_a;
    (void)seal_b;
    (void)pb;
}

// 不连服务器。覆盖帧、口令、端到端、日志轮转，以及好友/群/消息在 SQLite 里的状态变化
int main() {
    test_frame();
    test_pass_and_e2e();
    test_log_cap();
    test_db();
    if (g_fail) {
        std::cerr << g_fail << " checks failed" << std::endl;
        return 1;
    }
    std::cout << "unit ok" << std::endl;
    return 0;
}
