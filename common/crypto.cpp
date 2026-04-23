// OpenSSL 函数名即注释，报错即注释 ^_^

#include "crypto.h"

#include <stdexcept>
#include <cstring>
#include <openssl/rand.h>
#include <openssl/kdf.h>


Crypto::Crypto() noexcept
    : ecdh_keypr(nullptr, EVP_PKEY_free), peer_ecdh_pubkey(nullptr, EVP_PKEY_free) {}


Crypto::~Crypto() {
    if (!aeskey.empty()) {
        OPENSSL_cleanse(aeskey.data(), aeskey.size());
        // vector 析构会自动释放，无需 clear
    }
}


// ========== 生成 ECDH 密钥对 ==========
void Crypto::generate_ecdh_keypr() {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);     // 使用 X25519 曲线
    if (!ctx) throw std::runtime_error("Failed to create ECDH PKEY CTX");

    if (EVP_PKEY_keygen_init(ctx) != 1) {
        EVP_PKEY_CTX_free(ctx);
        throw std::runtime_error("Failed to init ECDH keygen");
    }

    EVP_PKEY* pkey{};
    if (EVP_PKEY_keygen(ctx, &pkey) != 1) {
        EVP_PKEY_CTX_free(ctx);
        throw std::runtime_error("Failed to generate ECDH keypair");
    }

    EVP_PKEY_CTX_free(ctx);
    ecdh_keypr.reset(pkey);
}


// ========== 获取 ECC 公钥 ==========
vecuc Crypto::get_ecdh_pubkey() const {
    if (!ecdh_keypr) return vecuc();

    size_t len{};
    if (!EVP_PKEY_get_raw_public_key(ecdh_keypr.get(), nullptr, &len)) {
        throw std::runtime_error("Failed to get ECDH public key length");
    }

    vecuc pubkey(len);
    if (!EVP_PKEY_get_raw_public_key(ecdh_keypr.get(), pubkey.data(), &len)) {
        throw std::runtime_error("Failed to get ECDH public key data");
    }

    return pubkey;
}


// ========== 设置对方的 ECC 公钥 ==========
void Crypto::set_peer_ecdh_pubkey(const vecuc& pubkey_raw) {
    if (pubkey_raw.empty()) throw std::runtime_error("Empty peer ECDH pubkey");

    EVP_PKEY* peer_pkey = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_X25519, nullptr, pubkey_raw.data(), pubkey_raw.size());
    if (!peer_pkey) throw std::runtime_error("Failed to create peer ECDH pubkey");

    peer_ecdh_pubkey.reset(peer_pkey);

    // 验证公钥有效性
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(peer_pkey, nullptr);
    if (!ctx) throw std::runtime_error("Failed to create PKEY CTX for peer pubkey");

    if (EVP_PKEY_public_check(ctx) != 1) {
        EVP_PKEY_CTX_free(ctx);
        peer_ecdh_pubkey.reset();
        throw std::runtime_error("ECDH public check failed");
    }

    EVP_PKEY_CTX_free(ctx);
}


// ========== 计算共享密钥并派生 AES 密钥 ==========
void Crypto::derive_shared_secret(const vecuc* salt_override) {
    if (!ecdh_keypr) throw std::runtime_error("Local ECDH keypair not set");
    if (!peer_ecdh_pubkey) throw std::runtime_error("Peer ECDH pubkey not set");

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(ecdh_keypr.get(), nullptr);
    if (!ctx) throw std::runtime_error("Failed to create ECDH CTX");

    if (EVP_PKEY_derive_init(ctx) != 1) {
        EVP_PKEY_CTX_free(ctx);
        throw std::runtime_error("Failed to init ECDH derive");
    }

    if (EVP_PKEY_derive_set_peer(ctx, peer_ecdh_pubkey.get()) != 1) {
        EVP_PKEY_CTX_free(ctx);
        throw std::runtime_error("Failed to set peer pubkey for ECDH");
    }

    size_t secret_len{};
    if (EVP_PKEY_derive(ctx, nullptr, &secret_len) != 1) {
        EVP_PKEY_CTX_free(ctx);
        throw std::runtime_error("Failed to get ECDH secret length");
    }

    vecuc shared_secret(secret_len);
    if (EVP_PKEY_derive(ctx, shared_secret.data(), &secret_len) != 1) {
        EVP_PKEY_CTX_free(ctx);
        throw std::runtime_error("Failed to derive ECDH shared secret");
    }

    EVP_PKEY_CTX_free(ctx);

    // ------------------------------
    // HKDF-SHA256 派生 32 字节 (256 位) AES 密钥
    // ------------------------------

    vecuc salt(16);
    if (salt_override && !salt_override->empty()) {
        salt = *salt_override;
    } else {
        if (!RAND_bytes(salt.data(), 16)) throw std::runtime_error("Failed to generate HKDF salt");
    }

    const char* info = "niyongyuancaibudaoinfoshenme";   // 上下文信息（没有也可以，但有了更安全）
    size_t info_len = strlen(info);

    vecuc derived_key(32);

    EVP_PKEY_CTX* kdf_ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
    if (!kdf_ctx) throw std::runtime_error("Failed to create HKDF CTX");

    if (EVP_PKEY_derive_init(kdf_ctx) != 1) {
        EVP_PKEY_CTX_free(kdf_ctx);
        throw std::runtime_error("Failed to init HKDF");
    }

    if (EVP_PKEY_CTX_hkdf_mode(kdf_ctx, EVP_PKEY_HKDEF_MODE_EXTRACT_AND_EXPAND) != 1) {
        EVP_PKEY_CTX_free(kdf_ctx);
        throw std::runtime_error("Failed to set HKDF mode");
    }

    if (EVP_PKEY_CTX_set_hkdf_md(kdf_ctx, EVP_sha256()) != 1) {
        EVP_PKEY_CTX_free(kdf_ctx);
        throw std::runtime_error("Failed to set HKDF digest");
    }

    if (EVP_PKEY_CTX_set1_hkdf_salt(kdf_ctx, salt.data(), salt.size()) != 1) {
        EVP_PKEY_CTX_free(kdf_ctx);
        throw std::runtime_error("Failed to set HKDF salt");
    }

    if (EVP_PKEY_CTX_set1_hkdf_key(kdf_ctx, shared_secret.data(), shared_secret.size()) != 1) {
        EVP_PKEY_CTX_free(kdf_ctx);
        throw std::runtime_error("Failed to set HKDF key");
    }

    if (EVP_PKEY_CTX_add1_hkdf_info(kdf_ctx, reinterpret_cast<const unsigned char*>(info), info_len) != 1) {
        EVP_PKEY_CTX_free(kdf_ctx);
        throw std::runtime_error("Failed to add HKDF info");
    }

    size_t outlen = 32;
    if (EVP_PKEY_derive(kdf_ctx, derived_key.data(), &outlen) != 1 || outlen != 32) {
        EVP_PKEY_CTX_free(kdf_ctx);
        throw std::runtime_error("Failed to derive AES key via HKDF");
    }

    EVP_PKEY_CTX_free(kdf_ctx);

    aeskey = std::move(derived_key);
}
