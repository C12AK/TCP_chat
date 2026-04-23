#ifndef CRYPTO_H
#define CRYPTO_H

#include "common.h"

#include <memory>
#include <openssl/evp.h>

// ECDH 会话类。持有一对 X25519 密钥及对端公钥，负责协商出 AES-256 会话密钥
class Crypto {
  private:
    // 本端 X25519 密钥对
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> ecdh_keypr;
    // 对端 X25519 公钥
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> peer_ecdh_pubkey;

  public:
    // 协商出的 AES-256 密钥（协商完成前为空）
    vecuc aeskey;

    explicit Crypto() noexcept;   // 仅构造无密钥的实例
    ~Crypto();                    // 析构时擦除 AES 密钥

    // 禁用拷贝，允许移动
    Crypto(const Crypto &) = delete;
    Crypto &operator=(const Crypto &) = delete;
    Crypto(Crypto &&) noexcept = default;
    Crypto &operator=(Crypto &&) noexcept = default;

    void generate_ecdh_keypr();                                      // 生成本端 ECC 密钥对
    vecuc get_ecdh_pubkey() const;                                   // 导出本端 ECC 公钥（raw bytes）
    void set_peer_ecdh_pubkey(const vecuc& pubkey_raw);              // 载入对端 ECC 公钥并校验
    void derive_shared_secret(const vecuc* salt_override = nullptr); // 计算共享密钥并 HKDF 出 AES 密钥
};

#endif // CRYPTO_H
