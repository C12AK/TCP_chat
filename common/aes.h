#ifndef AES_H
#define AES_H

#include "common.h"

#include <string>

// AES-256-GCM 无状态加解密。以 32 字节密钥作参数，线程安全
vecuc aes_encrypt(const vecuc& key, const vecuc& plain);
vecuc aes_decrypt(const vecuc& key, const vecuc& cipher);

// 兼容性重载：以 std::string 字节容器承载加解密结果
std::string aes_encrypt(const vecuc& key, const std::string& plainstr);
std::string aes_decrypt(const vecuc& key, const std::string& cipherstr);

#endif // AES_H
