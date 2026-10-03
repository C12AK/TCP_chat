#ifndef E2E_H
#define E2E_H

#include <string>

// 本机长期 X25519 密钥。私钥与公钥都是 32 字节。
bool e2e_generate(std::string& priv, std::string& pub);

// 用对方公钥封一封信。blob = 临时公钥 || 随机盐 || AES-256-GCM
bool e2e_seal(const std::string& recipient_pub, const std::string& plain, std::string& blob);

// 用自己的私钥拆开
bool e2e_open(const std::string& priv, const std::string& blob, std::string& plain);

#endif
