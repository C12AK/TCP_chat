#include "protocol.h"

#include <arpa/inet.h>
#include <cstring>
#include <stdexcept>

// 拼出一帧：1 字节类型，4 字节大端长度，然后是载荷。载荷超过上限则抛异常。
std::string build_frame(MsgType type, const std::string& payload) {
    if (payload.size() > FRAME_MAX_PAYLOAD) throw std::runtime_error("frame too large");

    std::string s;
    s.resize(FRAME_HEADER_LEN + payload.size());
    s[0] = static_cast<char>(type);
    uint32_t n = htonl(static_cast<uint32_t>(payload.size()));
    std::memcpy(s.data() + 1, &n, 4);
    if (!payload.empty()) std::memcpy(s.data() + FRAME_HEADER_LEN, payload.data(), payload.size());
    return s;
}

// 从 buf[off] 切一帧。半包返回空且不改 off；长度非法抛异常。成功则把 off 推过这一帧。
std::optional<Frame> parse_frame(const std::string& buf, std::size_t& off) {
    if (buf.size() - off < FRAME_HEADER_LEN) return std::nullopt;

    auto type_raw = static_cast<uint8_t>(buf[off]);
    uint32_t n_len = 0;
    std::memcpy(&n_len, buf.data() + off + 1, 4);
    uint32_t len = ntohl(n_len);
    if (len > FRAME_MAX_PAYLOAD) throw std::runtime_error("frame too large");
    if (buf.size() - off < FRAME_HEADER_LEN + len) return std::nullopt;

    Frame f;
    f.type = static_cast<MsgType>(type_raw);
    f.payload.assign(buf.data() + off + FRAME_HEADER_LEN, len);
    off += FRAME_HEADER_LEN + len;
    return f;
}

// 已消费超过 4KiB 且过半时才搬掉前缀。每帧都 memmove 会比短消息本身还贵。
void compact_buf(std::string& buf, std::size_t& off) {
    if (off == 0) return;
    if (off >= buf.size()) {
        buf.clear();
        off = 0;
        return;
    }
    if (off >= 4096 && off * 2 >= buf.size()) {
        buf.erase(0, off);
        off = 0;
    }
}

// 载荷写入 1 字节。
void W::u8(uint8_t v) { buf_.push_back(static_cast<char>(v)); }

// 按大端写入 8 字节。标准库没有可移植的 64 位 hton，所以从高位逐字节放。
void W::u64(uint64_t v) {
    char b[8];
    for (int i = 7; i >= 0; --i) {
        b[i] = static_cast<char>(v & 0xff);
        v >>= 8;
    }
    buf_.append(b, 8);
}

// 先写 4 字节大端长度，再写原始字节。超长则抛异常。
void W::str(std::string_view s) {
    if (s.size() > FRAME_MAX_PAYLOAD) throw std::runtime_error("string too large");
    uint32_t n = htonl(static_cast<uint32_t>(s.size()));
    buf_.append(reinterpret_cast<const char*>(&n), 4);
    buf_.append(s.data(), s.size());
}

// 读 1 字节。剩余不够返回 false，不抛异常。
bool R::u8(uint8_t& v) {
    if (in_.size() - off_ < 1) return false;
    v = static_cast<uint8_t>(in_[off_]);
    ++off_;
    return true;
}

// 按大端读 8 字节。剩余不够返回 false。
bool R::u64(uint64_t& v) {
    if (in_.size() - off_ < 8) return false;
    uint64_t x = 0;
    for (int i = 0; i < 8; ++i) x = (x << 8) | static_cast<uint8_t>(in_[off_ + i]);
    off_ += 8;
    v = x;
    return true;
}

// 读长度前缀字符串。长度不够、超上限或剩余字节不足都返回 false。
bool R::str(std::string& v) {
    if (in_.size() - off_ < 4) return false;
    uint32_t n_len = 0;
    std::memcpy(&n_len, in_.data() + off_, 4);
    uint32_t len = ntohl(n_len);
    off_ += 4;
    if (len > FRAME_MAX_PAYLOAD || in_.size() - off_ < len) return false;
    v.assign(in_.data() + off_, len);
    off_ += len;
    return true;
}
