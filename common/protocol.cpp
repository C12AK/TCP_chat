#include "protocol.h"

#include <arpa/inet.h>
#include <cstring>
#include <stdexcept>


// ==================== KA 帧 ====================
std::string build_ka_frame(const unsigned char* payload, std::size_t len) {
    std::string s;
    s.reserve(KA_HEADER_LEN + len);
    uint32_t n_len = htonl(static_cast<uint32_t>(len));
    s.append(reinterpret_cast<const char*>(&n_len), sizeof(n_len));
    s.append(reinterpret_cast<const char*>(payload), len);
    return s;
}


std::string build_ka_frame(const vecuc& payload) {
    return build_ka_frame(payload.data(), payload.size());
}


std::string build_ka_frame(const std::string& payload) {
    return build_ka_frame(reinterpret_cast<const unsigned char*>(payload.data()), payload.size());
}


std::optional<std::string> parse_ka_frame(const std::string& buf, std::size_t& off) {
    if (buf.size() - off < KA_HEADER_LEN) return std::nullopt;

    uint32_t n_len;
    std::memcpy(&n_len, buf.data() + off, sizeof(n_len));
    uint32_t len = ntohl(n_len);

    // 限制单个 KA 帧最大长度，避免恶意长度导致分配过大
    if (len > (1u << 20)) throw std::runtime_error("KA frame too large");

    if (buf.size() - off < KA_HEADER_LEN + len) return std::nullopt;

    std::string payload(buf.data() + off + KA_HEADER_LEN, len);
    off += KA_HEADER_LEN + len;
    return payload;
}


std::optional<std::string> parse_ka_frame(std::string& buf) {
    std::size_t off = 0;
    auto payload = parse_ka_frame(static_cast<const std::string&>(buf), off);
    if (payload) buf.erase(0, off);
    return payload;
}


// ==================== Chat 帧 ====================
std::string build_chat_frame(const std::string& c_to, const std::string& c_msg) {
    uint16_t n_tolen = htons(static_cast<uint16_t>(c_to.size()));
    uint32_t n_msglen = htonl(static_cast<uint32_t>(c_msg.size()));

    std::string pck;
    pck.reserve(CHAT_HEADER_LEN + c_to.size() + c_msg.size());
    pck.append(reinterpret_cast<const char*>(&n_tolen), sizeof(n_tolen));
    pck.append(reinterpret_cast<const char*>(&n_msglen), sizeof(n_msglen));
    pck += c_to;
    pck += c_msg;
    return pck;
}


std::optional<ChatFrame> parse_chat_frame(const std::string& buf, std::size_t& off) {
    if (buf.size() - off < CHAT_HEADER_LEN) return std::nullopt;

    uint16_t n_tolen;
    uint32_t n_msglen;
    std::memcpy(&n_tolen, buf.data() + off, sizeof(n_tolen));
    std::memcpy(&n_msglen, buf.data() + off + sizeof(n_tolen), sizeof(n_msglen));
    std::size_t tolen = ntohs(n_tolen);
    std::size_t msglen = ntohl(n_msglen);

    // 同上，限制最大载荷
    if (tolen > (1u << 16) || msglen > (1u << 24)) throw std::runtime_error("Chat frame too large");

    std::size_t total = CHAT_HEADER_LEN + tolen + msglen;
    if (buf.size() - off < total) return std::nullopt;

    ChatFrame f;
    f.c_to.assign(buf.data() + off + CHAT_HEADER_LEN, tolen);
    f.c_msg.assign(buf.data() + off + CHAT_HEADER_LEN + tolen, msglen);
    off += total;
    return f;
}


std::optional<ChatFrame> parse_chat_frame(std::string& buf) {
    std::size_t off = 0;
    auto f = parse_chat_frame(static_cast<const std::string&>(buf), off);
    if (f) buf.erase(0, off);
    return f;
}


void compact_buf(std::string& buf, std::size_t& off) {
    if (off == 0) return;
    if (off >= buf.size()) {
        buf.clear();
        off = 0;
        return;
    }
    // 已消费超过 4KiB 且占一半以上才搬移，避免每帧都 memmove
    if (off >= 4096 && off * 2 >= buf.size()) {
        buf.erase(0, off);
        off = 0;
    }
}
