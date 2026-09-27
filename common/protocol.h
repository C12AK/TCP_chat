#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "common.h"

#include <string>
#include <optional>
#include <cstdint>

// ============================================================
// 应用层协议：
//
//   1. 握手阶段（KA 帧）：四字节大端长度 + 载荷
//        C -> S : 用户名 (raw bytes)
//        S -> C : 服务器 ECDH 公钥
//        C -> S : 客户端 ECDH 公钥
//
//   2. 聊天阶段（Chat 帧）：
//        2 字节大端 tolen + 4 字节大端 msglen + AES(to) + AES(msg)
//        （from / to / msg 均已被 AES-256-GCM 加密）
//
//   两种帧都支持粘包与分块：由帧头指明载荷长度
// ============================================================

constexpr std::size_t KA_HEADER_LEN = 4;
constexpr std::size_t CHAT_HEADER_LEN = 2 + 4;

// 构建 KA 帧
std::string build_ka_frame(const unsigned char* payload, std::size_t len);
std::string build_ka_frame(const vecuc& payload);
std::string build_ka_frame(const std::string& payload);

// 尝试从缓冲区解析一个 KA 帧
//   成功：返回载荷，并擦除 buf 中对应字节
//   数据不足：返回 nullopt，buf 保持不变
//   协议错误：抛出 std::runtime_error
std::optional<std::string> parse_ka_frame(std::string& buf);

// 从 buf[off..] 解析一个 KA 帧。成功则推进 off，不移动剩余字节
std::optional<std::string> parse_ka_frame(const std::string& buf, std::size_t& off);

// 构建 Chat 帧
std::string build_chat_frame(const std::string& c_to, const std::string& c_msg);

// 尝试从缓冲区解析一个 Chat 帧。语义同 parse_ka_frame
//   成功时：c_to / c_msg 为原始（仍为密文）字节串
struct ChatFrame {
    std::string c_to;
    std::string c_msg;
};
std::optional<ChatFrame> parse_chat_frame(std::string& buf);

// 从 buf[off..] 解析一个 Chat 帧。成功则推进 off，不移动剩余字节
std::optional<ChatFrame> parse_chat_frame(const std::string& buf, std::size_t& off);

// 丢掉 [0, off) 已消费字节；off==size 时直接 clear
void compact_buf(std::string& buf, std::size_t& off);

#endif // PROTOCOL_H
