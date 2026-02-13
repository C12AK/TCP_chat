#include "AES_GCM.h"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <cstring>
#include <string>
#include <stdexcept>

AESGCM::AESGCM() {
    key.resize(KEY_SIZE);
    if (!RAND_bytes(key.data(), KEY_SIZE)) {
        throw std::runtime_error("Failed to generate random key");
    }
}

AESGCM::~AESGCM() {
    // OpenSSL 资源会在 encrypt/decrypt 中自动清理
    // 如果有持久化的 EVP_CIPHER_CTX，需要在这里清理
}

// 移动构造函数（可选）
AESGCM::AESGCM(AESGCM&& other) noexcept 
    : key(std::move(other.key)) {}

AESGCM& AESGCM::operator=(AESGCM&& other) noexcept {
    if (this != &other) {
        key = std::move(other.key);
    }
    return *this;
}

std::vector<unsigned char> AESGCM::encrypt(const std::string& plaintext) {
    std::vector<unsigned char> iv(IV_SIZE);
    if (!RAND_bytes(iv.data(), IV_SIZE)) {
        throw std::runtime_error("Failed to generate IV");
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("Failed to create context");

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key.data(), iv.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Failed to initialize encryption");
    }

    std::vector<unsigned char> ciphertext(plaintext.size() + EVP_MAX_BLOCK_LENGTH);
    int len;
    if (EVP_EncryptUpdate(ctx, ciphertext.data(), &len, 
                          reinterpret_cast<const unsigned char*>(plaintext.data()), 
                          plaintext.size()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Encryption failed");
    }
    int ciphertext_len = len;

    if (EVP_EncryptFinal_ex(ctx, ciphertext.data() + len, &len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Finalization failed");
    }
    ciphertext_len += len;

    std::vector<unsigned char> tag(16);
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Failed to get tag");
    }

    EVP_CIPHER_CTX_free(ctx);

    // 返回格式: IV + ciphertext + tag
    std::vector<unsigned char> result;
    result.reserve(IV_SIZE + ciphertext_len + 16);
    result.insert(result.end(), iv.begin(), iv.end());
    result.insert(result.end(), ciphertext.begin(), ciphertext.begin() + ciphertext_len);
    result.insert(result.end(), tag.begin(), tag.end());
    
    return result;
}

// 解密函数（你需要实现）
std::vector<unsigned char> AESGCM::decrypt(const std::vector<unsigned char>& ciphertext) {
    if (ciphertext.size() < IV_SIZE + 16) {
        throw std::runtime_error("Ciphertext too short");
    }
    
    // 提取 IV、密文、tag
    std::vector<unsigned char> iv(ciphertext.begin(), ciphertext.begin() + IV_SIZE);
    std::vector<unsigned char> tag(ciphertext.end() - 16, ciphertext.end());
    std::vector<unsigned char> encrypted_data(ciphertext.begin() + IV_SIZE, ciphertext.end() - 16);
    
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("Failed to create context");
    
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key.data(), iv.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Failed to initialize decryption");
    }
    
    // 设置 tag
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Failed to set tag");
    }
    
    std::vector<unsigned char> plaintext(encrypted_data.size() + EVP_MAX_BLOCK_LENGTH);
    int len;
    if (EVP_DecryptUpdate(ctx, plaintext.data(), &len, encrypted_data.data(), encrypted_data.size()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Decryption failed");
    }
    int plaintext_len = len;
    
    if (EVP_DecryptFinal_ex(ctx, plaintext.data() + len, &len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Authentication failed");
    }
    plaintext_len += len;
    
    EVP_CIPHER_CTX_free(ctx);
    
    plaintext.resize(plaintext_len);
    return plaintext;
}