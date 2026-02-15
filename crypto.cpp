// OpenSSL 函数名即注释，报错即注释 ^_^

#include "crypto.h"
#include <stdexcept>


// 构造函数仅创建空对象
Crypto::Crypto() noexcept : keypr(nullptr, RSA_free){}


// ========== 服务端：产生 RSA 密钥对 ==========
void Crypto::generate_key_pair() noexcept {
    BIGNUM* bn = BN_new();
    BN_set_word(bn, RSA_F4);    // 设置 65537 位的公钥指数

    RSA* tmp_rsa = RSA_new();
    if (tmp_rsa && RSA_generate_key_ex(tmp_rsa, 2048, bn, nullptr) == 1) {
        keypr.reset(tmp_rsa);
    } else if (tmp_rsa) {
        RSA_free(tmp_rsa);
    }

    BN_free(bn);
}


// ========== 服务端：获取公钥 DER ==========
vecuc Crypto::get_pubkey_der() const noexcept {
    if (!keypr) return vecuc();

    unsigned char* derptr = nullptr;
    int len = i2d_RSAPublicKey(keypr.get(), &derptr);      // 此函数会为 derptr 指向的串分配空间
    if (len <= 0 || derptr == nullptr) return vecuc();

    vecuc res(derptr, derptr + len);
    OPENSSL_free(derptr);
    return res;
}


// ========== 客户端：设置公钥 ==========
void Crypto::set_pubkey_from_der(const vecuc& der) {
    if (der.empty()) throw std::runtime_error("Empty DER");

    const unsigned char* derptr = der.data();
    RSA* pubkey = d2i_RSAPublicKey(nullptr, &derptr, der.size());   // 此函数会修改指针，所以必须创建 derptr
    if (!pubkey) throw std::runtime_error("Failed to set RSA pubkey from DER");

    keypr.reset(pubkey);
}


// ========== 客户端：RSA 加密 ==========
vecuc Crypto::rsa_encrypt(const vecuc& plain) const {
    if (!keypr || plain.empty()) throw std::runtime_error("Invalid RSA pubkey or empty plaintext");
    if (plain.size() + 11ul > static_cast<size_t>(RSA_size(keypr.get()))) throw std::runtime_error("Plaintext too long");

    vecuc cipher(RSA_size(keypr.get()));
    int len = RSA_public_encrypt(
        static_cast<int>(plain.size()),
        plain.data(),
        cipher.data(),
        keypr.get(),
        RSA_PKCS1_PADDING
    );

    if (len <= 0) throw std::runtime_error("RSA encryption error");

    cipher.resize(len);
    return cipher;
}


// ========== 服务端：RSA 解密 ==========
vecuc Crypto::rsa_decrypt(const vecuc& cipher) const {
    if (!keypr || cipher.empty()) throw std::runtime_error("Invalid RSA privkey or empty ciphertext");
    if (cipher.size() > static_cast<size_t>(RSA_size(keypr.get()))) throw std::runtime_error("Cipher too long");

    vecuc plain(RSA_size(keypr.get()));
    int len = RSA_private_decrypt(
        static_cast<int>(cipher.size()),
        cipher.data(),
        plain.data(),
        keypr.get(),
        RSA_PKCS1_PADDING
    );

    if (len <= 0) throw std::runtime_error("RSA decryption error");
    return plain;
}


// ========== 客户端：生成随机 AES 密钥 ==========
vecuc Crypto::generate_rand_aeskey() const {
    vecuc key(32);
    if (!RAND_bytes(key.data(), 32)) throw std::runtime_error("Failed to generate AES key");
    return key;
}


// ========== AES 加密 ==========
vecuc Crypto::aes_encrypt(const vecuc& plain, const vecuc& key) const {
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

    vecuc tag(16);
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Failed to GET AES authentication tag");
    }

    EVP_CIPHER_CTX_free(ctx);

    vecuc(res);
    res.reserve(iv.size() + totlen + tag.size());
    res.insert(res.end(), iv.begin(), iv.end());
    res.insert(res.end(), cipher.begin(), cipher.begin() + totlen);
    res.insert(res.end(), tag.begin(), tag.end());
    return res;
}


// ========== AES 解密 ==========
vecuc Crypto::aes_decrypt(const vecuc& cipher, const vecuc& key) const {
    if (cipher.size() < 28 || key.size() != 32) throw std::runtime_error("Invalid length of AES key or cipher");

    vecuc iv(cipher.begin(), cipher.begin() + 12), tag(cipher.end() - 16, cipher.end()), 
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
    if (EVP_DecryptUpdate(ctx, plain.data(), &len, cipher.data(), static_cast<int>(cipher.size())) != 1) {
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