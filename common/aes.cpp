#include "aes.h"

#include <stdexcept>
#include <openssl/evp.h>
#include <openssl/rand.h>


// ========== AES 加密 ==========
// 输出布局是 12 字节随机 IV、密文、16 字节 GCM 标签。同一明文每次结果不同。
vecuc aes_encrypt(const vecuc& key, const vecuc& plain) {
    if (key.size() != 32) throw std::runtime_error("Invalid AES key length");

    vecuc iv(12);
    if (!RAND_bytes(iv.data(), 12)) throw std::runtime_error("Failed to generate IV");

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("Failed to create AES CTX");

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key.data(), iv.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("AES encryption INIT error");
    }

    vecuc cipher(plain.size() + EVP_MAX_BLOCK_LENGTH);
    int len;
    if (EVP_EncryptUpdate(ctx, cipher.data(), &len, plain.data(), static_cast<int>(plain.size())) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("AES encryption UPDATE error");
    }
    int totlen = len;

    if (EVP_EncryptFinal_ex(ctx, cipher.data() + len, &len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("AES encryption FINAL error");
    }
    totlen += len;

    vecuc tag(16);  // 认证标签。解密时对不上就说明密钥错或密文被改过
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Failed to GET AES authentication tag");
    }

    EVP_CIPHER_CTX_free(ctx);

    vecuc res;
    res.reserve(iv.size() + totlen + tag.size());
    res.insert(res.end(), iv.begin(), iv.end());
    res.insert(res.end(), cipher.begin(), cipher.begin() + totlen);
    res.insert(res.end(), tag.begin(), tag.end());
    return res;
}


// ========== AES 解密 ==========
// 输入必须是加密函数的布局。标签校验失败抛异常，不返回半截明文。
vecuc aes_decrypt(const vecuc& key, const vecuc& cipher) {
    if (cipher.size() < 28 || key.size() != 32) throw std::runtime_error("Invalid length of AES key or cipher");

    vecuc iv(cipher.begin(), cipher.begin() + 12),
        tag(cipher.end() - 16, cipher.end()),
        data(cipher.begin() + 12, cipher.end() - 16);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("Failed to create AES CTX");

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key.data(), iv.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("AES decryption INIT error");
    }

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Failed to SET AES authentication tag");
    }

    vecuc plain(cipher.size() + EVP_MAX_BLOCK_LENGTH);
    int len;
    if (EVP_DecryptUpdate(ctx, plain.data(), &len, data.data(), static_cast<int>(data.size())) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("AES decryption UPDATE error");
    }
    int totlen = len;

    if (EVP_DecryptFinal_ex(ctx, plain.data() + len, &len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("AES authentication (decryption FINAL) failed");
    }
    totlen += len;

    EVP_CIPHER_CTX_free(ctx);
    plain.resize(totlen);
    return plain;
}


// ========== string 重载 ==========
// 只做 string 和 vecuc 的字节转换，算法在上面两个函数里。
std::string aes_encrypt(const vecuc& key, const std::string& plainstr) {
    vecuc tmp(plainstr.begin(), plainstr.end());
    vecuc res = aes_encrypt(key, tmp);
    return std::string(res.begin(), res.end());
}


// string 版本的解密，对应上面的加密重载。
std::string aes_decrypt(const vecuc& key, const std::string& cipherstr) {
    vecuc tmp(cipherstr.begin(), cipherstr.end());
    vecuc res = aes_decrypt(key, tmp);
    return std::string(res.begin(), res.end());
}
