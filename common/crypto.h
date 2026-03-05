#ifndef CRYPTO_H
#define CRYPTO_H

#include <vector>
#include <memory>
#include <openssl/evp.h>

#define vecuc std::vector<unsigned char> // [NOTICE]

// 统一的加密工具类。对于非文本的 “字符串” ，最好用 vector<unsigned char>
class Crypto {
  private:
    // ECC 密钥对（服务端长期密钥 + 客户端临时密钥）
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> ecdh_keypr;
    // 对方的 ECC 公钥（服务端存客户端的，客户端存服务端的）
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> peer_ecdh_pubkey;

  public:
    vecuc aeskey;  

    explicit Crypto() noexcept;   // 构造函数仅构造无密钥的实例
    ~Crypto();                    // 析构函数用于安全擦除 AES 密钥内存

    // 禁用拷贝构造和赋值，允许移动构造和赋值
    Crypto(const Crypto &) = delete;
    Crypto &operator=(const Crypto &) = delete;
    Crypto(Crypto &&) noexcept = default;
    Crypto &operator=(Crypto &&) noexcept = default;

    // =========== ECDH ===========
    void generate_ecdh_keypr();                           // 生成 ECC 密钥对
    vecuc get_ecdh_pubkey() const;                        // 获取 ECC 公钥
    void set_peer_ecdh_pubkey(const vecuc &pubkey_der);   // 设置对方的 ECC 公钥
    void derive_shared_secret(const vecuc *salt_override = nullptr);  // 计算共享密钥并派生 AES 密钥，可选传入盐值确保双方一致

    // =========== AES ===========
    vecuc aes_encrypt(const vecuc &plain) const;                  // AES 加密
    vecuc aes_decrypt(const vecuc &cipher) const;                 // AES 解密
    std::string aes_encrypt(const std::string &plainstr) const;   // 兼容性重载
    std::string aes_decrypt(const std::string &cipherstr) const;  // 兼容性重载
};

#endif // CRYPTO_H