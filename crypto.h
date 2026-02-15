#ifndef CRYPTO_H
#define CRYPTO_H

#include <vector>
#include <memory>
#include <openssl/rsa.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#define vecuc std::vector<unsigned char>  // [NOTICE]


// 统一的加密工具类。对于非文本的 “字符串” ，最好用 vector<unsigned char>
class Crypto {
private:
    // RSA 类是存储密钥对的容器。服务端持有完整的 RSA 密钥对，客户端只持有 RSA 公钥
    // decltype: 自动推导 RSA_free (释放 RSA 密钥的函数) 的类型
    std::unique_ptr<RSA, decltype(&RSA_free)> keypr;
    
public:
    // 构造无密钥的实例
    explicit Crypto() noexcept;
    
    // 禁用拷贝构造和赋值，允许移动构造和赋值
    Crypto(const Crypto&) = delete;
    Crypto& operator=(const Crypto&) = delete;
    Crypto(Crypto&&) noexcept = default;
    Crypto& operator=(Crypto&&) noexcept = default;
    
    
    // ========== 服务端功能 ==========
    void generate_key_pair() noexcept;                 // 服务端产生密钥对
    vecuc get_pubkey_der() const noexcept;              // 获取 RSA 公钥 (DER 二进制格式)
    vecuc rsa_decrypt(const vecuc& cipher) const;       // RSA 解密
    

    // ========== 客户端功能 ==========
    void set_pubkey_from_der(const vecuc& der);         // 设置 RSA 公钥
    vecuc generate_rand_aeskey() const;                 // 生成 AES 密钥
    vecuc rsa_encrypt(const vecuc& plain) const;        // RSA 加密
    

    // =========== 通用功能 ===========
    vecuc aes_encrypt(const vecuc& plain, const vecuc& key) const;    // AES 加密
    vecuc aes_decrypt(const vecuc& cipher, const vecuc& key) const;   // AES 解密
};

#endif  // CRYPTO_H