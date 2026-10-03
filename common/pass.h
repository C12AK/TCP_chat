#ifndef PASS_H
#define PASS_H

#include <string>

// 生成 16 字节盐，并用 PBKDF2-HMAC-SHA256 导出 32 字节口令哈希
bool make_pass_hash(const std::string& password, std::string& salt, std::string& hash);

// 用同一盐重算哈希，与保存的哈希比较
bool check_pass_hash(const std::string& password, const std::string& salt, const std::string& hash);

#endif
