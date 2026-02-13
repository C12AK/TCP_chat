#ifndef AES_GCM_H
#define AES_GCM_H

#include <string>
#include <stdexcept>

// 前向声明：避免头文件包含大型第三方库，否则所有用户代码都会包含 OpenSSL 库，编译慢
// 只在实现文件 AES_GCM.cpp 真正包含 OpenSSL ，其中存在 evp_cipher_ctx_st 真正的实现
struct evp_cipher_ctx_st;
typedef struct evp_cipher_ctx_st EVP_CIPHER_CTX;

class AES {
  private:
    std::string key;                    // 一般而言用 vector<unsigned char> 更好，但这里 string 更方便嵌入已有的收发逻辑
    static constexpr size_t KEYSZ = 32; // 256 位密钥
    static constexpr size_t IVSZ = 12;  // 96 位初始向量

  public:
    AES();
    ~AES();

    // 禁用拷贝，避免 OpenSSL 上下文拷贝（会导致密钥相同等后果）
    AES(const AES&) = delete;
    AES& operator=(const AES&) = delete;

    // 允许移动
    AES(AES&&) noexcept;
    AES& operator=(AES&&) noexcept;

    std::string encrypt(const std::string& plain);
    std::string decrypt(const std::string& cipher);
};

#endif  // AES_GCM_H