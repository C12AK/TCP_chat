#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// 统一帧：1 字节类型 + 4 字节大端长度 + 载荷。
// 载荷由 W / R 按字段顺序读写。服务器不解析密文块内部。
//
// 各类型载荷（顺序固定）：
//   Register            str 用户名, str 密码, str 公钥(32)
//   Login               str 用户名, str 密码
//   LoginOk/RegisterOk  u64 用户 id
//   LoginFail/Kick           str 原因
//   Error/QueueBusy          str 原因。聊天失败时再跟 u64 nonce、u64 消息 id（尚未入库则为 0）
//   Heartbeat           空。只用来刷新空闲计时
//   SyncReq             u64 本地已有的最大消息 id
//   SyncBatch           u64 条数，然后重复 Push 的字段
//   ChatSend            u64 会话, u64 客户端 nonce, u64 份数，重复 {u64 收件人, str 密文}
//   ChatAck             u64 nonce, u64 服务器消息 id
//   Push                u64 消息 id, u64 会话, u64 发送者, u64 时间, str 密文
//   Receipt             u8 种类(1 送达 2 已读), u64 会话, u64 消息 id, u64 回执发出者
//   FriendSearch        str 完整用户名
//   FriendSearchResult  u8 关系(0 无 1 已申请 2 待我处理 3 好友 4 自己 255 没有), u64 id, str 名
//   FriendRequest       u64 对方 id。空载荷的同类型帧表示申请已被收下
//   FriendRespond       u64 申请者, u8(1 同意 / 0 拒绝)
//   FriendDelete        u64 对方 id
//   ConvList            u64 版本, u64 个数，重复 {u64 id, u8 种类, str 标题, u64 已读, u64 最新, u64 群主, u64 成员数，重复 {u64 id, str 名, str 公钥}}
//   GroupCreate         str 群名
//   GroupCreateOk       u64 会话 id, str 群名
//   GroupInvite         u64 会话, u64 被邀请者
//   GroupLeave/GroupDissolve  u64 会话
// 会话种类：0 私聊，1 群，2 与「系统」的会话。版本号只增，旧的 ConvList 直接丢掉。

enum class MsgType : uint8_t {
    Register = 1,
    Login = 2,
    LoginOk = 3,
    LoginFail = 4,
    Kick = 5,
    Heartbeat = 6,
    SyncReq = 7,
    SyncBatch = 8,
    ChatSend = 9,
    ChatAck = 10,
    Push = 11,
    Receipt = 12,
    FriendSearch = 13,
    FriendSearchResult = 14,
    FriendRequest = 15,
    FriendRespond = 16,
    FriendDelete = 17,
    ConvList = 18,
    GroupCreate = 19,
    GroupCreateOk = 20,
    GroupInvite = 21,
    GroupLeave = 22,
    GroupDissolve = 23,
    QueueBusy = 24,
    Error = 25,
    RegisterOk = 26,
};

constexpr std::size_t FRAME_HEADER_LEN = 1 + 4;
constexpr std::size_t FRAME_MAX_PAYLOAD = 8u << 20;

struct Frame {
    MsgType type{};
    std::string payload;
};

std::string build_frame(MsgType type, const std::string& payload);

// 成功则推进 off。数据不够返回 nullopt。类型或长度非法抛出 std::runtime_error
std::optional<Frame> parse_frame(const std::string& buf, std::size_t& off);

// 已消费超过 4KiB 且过半时搬掉前缀，避免入站缓冲只增不减
void compact_buf(std::string& buf, std::size_t& off);

// 载荷写入
class W {
  public:
    void u8(uint8_t v);
    void u64(uint64_t v);
    void str(std::string_view s);

    const std::string& data() const { return buf_; }
    // 把已写入的载荷交出去，调用后内部缓冲为空
    std::string take() { return std::move(buf_); }

  private:
    std::string buf_;
};

// 载荷读取。失败返回 false，不抛异常
class R {
  public:
    explicit R(std::string_view in) : in_(in) {}

    bool u8(uint8_t& v);
    bool u64(uint64_t& v);
    bool str(std::string& v);
    bool empty() const { return off_ == in_.size(); }

  private:
    std::string_view in_;
    std::size_t off_ = 0;
};

#endif
