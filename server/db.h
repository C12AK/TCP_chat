#ifndef DB_H
#define DB_H

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// 服务端 SQLite。所有方法自带锁，可从线程池调用。
class Database {
  public:
    static constexpr uint64_t kSystemId = 1;

    struct User {
        uint64_t id = 0;
        std::string username;
        std::string pubkey;
    };

    struct Conv {
        uint64_t id = 0;
        int kind = 0;  // 0 私聊  1 群  2 系统
        std::string title;
        uint64_t owner_id = 0;
        uint64_t last_read_id = 0;
        uint64_t latest_id = 0;
    };

    struct Copy {
        uint64_t msg_id = 0;
        uint64_t conv_id = 0;
        uint64_t sender_id = 0;
        uint64_t ts_ms = 0;
        std::string ciphertext;
    };

    struct MemberSeal {
        uint64_t user_id = 0;
        std::string ciphertext;
    };

    explicit Database(const std::string& path);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // 注册。成功时 out_id 为新用户 id，并已建好与系统的会话
    bool register_user(const std::string& username, const std::string& password,
                       const std::string& pubkey, uint64_t& out_id, std::string& err);

    // 核对口令。成功时 out 带 id、用户名和长期公钥。系统账号不能登录
    bool verify_login(const std::string& username, const std::string& password,
                      User& out, std::string& err);

    bool user_by_id(uint64_t id, User& out);                  // 找不到返回 false
    bool user_by_name(const std::string& username, User& out);

    // 该用户能看到的会话（系统、私聊、群）
    // 已解散或已删除好友的会话不在这里
    std::vector<Conv> list_convs(uint64_t user_id);

    bool conv_kind(uint64_t conv_id, int& kind, uint64_t& owner_id); // 会话已关闭时返回 false
    bool is_member(uint64_t conv_id, uint64_t user_id);
    std::vector<uint64_t> members(uint64_t conv_id);

    // 写入一条消息的多份密文。nonce 重复则返回已有 msg_id，dup=true
    bool insert_message(uint64_t conv_id, uint64_t sender_id, uint64_t nonce, uint64_t ts_ms,
                        const std::vector<MemberSeal>& copies, uint64_t& msg_id, bool& dup,
                        std::string& err);

    // 该用户 id 大于 after_id 的密文，按消息 id 升序，最多 limit 条
    std::vector<Copy> sync_for(uint64_t user_id, uint64_t after_id, int limit);

    bool set_read(uint64_t user_id, uint64_t conv_id, uint64_t msg_id); // 只把已读位置往更大的 id 推

    bool message_info(uint64_t msg_id, uint64_t& conv_id, uint64_t& sender_id);

    // 精确用户名。relation: 0 没有  1 已申请  2 对方申请了我  3 已是好友  4 是自己
    bool search_user(uint64_t self, const std::string& username, User& out, int& relation);

    bool add_request(uint64_t from, uint64_t to, std::string& err); // 已是好友或已申请过则失败
    // 同意时建立私聊会话并写入 conv_id；拒绝则只删申请
    bool respond_request(uint64_t self, uint64_t requester, bool accept, uint64_t& conv_id, std::string& err);
    bool delete_friend(uint64_t a, uint64_t b, uint64_t& conv_id); // 双方都退出该私聊，历史密文仍留在库里

    uint64_t system_conv(uint64_t user_id); // 没有就创建。注册时已经建过

    bool create_group(uint64_t owner, const std::string& title, uint64_t& conv_id, std::string& err);
    bool invite_group(uint64_t conv_id, uint64_t by, uint64_t user_id, std::string& err); // by 必须已在群里，且与 user_id 是好友
    bool leave_group(uint64_t conv_id, uint64_t user_id, std::string& err);               // 群主不能走这条，只能解散
    bool dissolve_group(uint64_t conv_id, uint64_t owner, std::vector<uint64_t>& former, std::string& err);

  private:
    bool exec(const char* sql);
    uint64_t ensure_system_conv(uint64_t user_id);

    struct sqlite3* db = nullptr;
    std::recursive_mutex mu; // 方法会互相调用，且各自都加锁，同一线程必须能加多次
};

#endif
