#include "e2e.h"

#include "aes.h"

#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

namespace {

constexpr int kPub = 32;   // X25519 公钥、私钥都是 32 字节
constexpr int kSalt = 16;  // 每封信单独的随机盐

// 导出原始公钥。第一次调用只问长度，第二次才把字节写入 out。
bool raw_pub(EVP_PKEY* key, std::string& out) {
    size_t len = 0;
    if (!EVP_PKEY_get_raw_public_key(key, nullptr, &len) || len != kPub) return false;
    out.assign(len, '\0');
    return EVP_PKEY_get_raw_public_key(key, reinterpret_cast<unsigned char*>(out.data()), &len) == 1;
}

// 导出原始私钥，步骤和 raw_pub 相同。
bool raw_priv(EVP_PKEY* key, std::string& out) {
    size_t len = 0;
    if (!EVP_PKEY_get_raw_private_key(key, nullptr, &len) || len != kPub) return false;
    out.assign(len, '\0');
    return EVP_PKEY_get_raw_private_key(key, reinterpret_cast<unsigned char*>(out.data()), &len) == 1;
}

// 生成一把 X25519 密钥。失败返回空，调用方负责 EVP_PKEY_free。
EVP_PKEY* gen_key() {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    if (!ctx) return nullptr;
    EVP_PKEY* key = nullptr;
    if (EVP_PKEY_keygen_init(ctx) != 1 || EVP_PKEY_keygen(ctx, &key) != 1) {
        EVP_PKEY_CTX_free(ctx);
        return nullptr;
    }
    EVP_PKEY_CTX_free(ctx);
    return key;
}

// 把 32 字节公钥装回 OpenSSL 密钥对象。长度不对返回空。
EVP_PKEY* load_pub(const std::string& raw) {
    if (raw.size() != kPub) return nullptr;
    return EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr,
                                       reinterpret_cast<const unsigned char*>(raw.data()), raw.size());
}

// 把 32 字节私钥装回密钥对象。长度不对返回空。
EVP_PKEY* load_priv(const std::string& raw) {
    if (raw.size() != kPub) return nullptr;
    return EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr,
                                        reinterpret_cast<const unsigned char*>(raw.data()), raw.size());
}

// 用己方私钥和对方公钥做 X25519。两边算出的 secret 相同，链路上只有公钥。
bool ecdh(EVP_PKEY* mine, EVP_PKEY* peer, std::string& secret) {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(mine, nullptr);
    if (!ctx) return false;
    if (EVP_PKEY_derive_init(ctx) != 1 || EVP_PKEY_derive_set_peer(ctx, peer) != 1) {
        EVP_PKEY_CTX_free(ctx);
        return false;
    }
    size_t len = 0;
    if (EVP_PKEY_derive(ctx, nullptr, &len) != 1) {
        EVP_PKEY_CTX_free(ctx);
        return false;
    }
    secret.assign(len, '\0');
    int rc = EVP_PKEY_derive(ctx, reinterpret_cast<unsigned char*>(secret.data()), &len);
    EVP_PKEY_CTX_free(ctx);
    return rc == 1;
}

// 用盐和共享秘密做 HKDF-SHA256，导出 32 字节 AES 密钥。
// info 把密钥绑在本协议上，别的程序即使算出同一个秘密也得不到这把密钥。
bool hkdf_key(const std::string& secret, const std::string& salt, std::string& key) {
    key.assign(32, '\0');
    const char* info = "tcpchat-e2e-v1";  // 14 字节，和下面 add1 的长度一致
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
    if (!ctx) return false;
    bool ok = EVP_PKEY_derive_init(ctx) == 1
        && EVP_PKEY_CTX_hkdf_mode(ctx, EVP_PKEY_HKDEF_MODE_EXTRACT_AND_EXPAND) == 1
        && EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha256()) == 1
        && EVP_PKEY_CTX_set1_hkdf_salt(ctx, reinterpret_cast<const unsigned char*>(salt.data()), static_cast<int>(salt.size())) == 1
        && EVP_PKEY_CTX_set1_hkdf_key(ctx, reinterpret_cast<const unsigned char*>(secret.data()), static_cast<int>(secret.size())) == 1
        && EVP_PKEY_CTX_add1_hkdf_info(ctx, reinterpret_cast<const unsigned char*>(info), 14) == 1;
    size_t outlen = 32;
    ok = ok && EVP_PKEY_derive(ctx, reinterpret_cast<unsigned char*>(key.data()), &outlen) == 1 && outlen == 32;
    EVP_PKEY_CTX_free(ctx);
    return ok;
}

}  // namespace

// 生成长期密钥对。priv 和 pub 都写成 32 字节，失败返回 false。
bool e2e_generate(std::string& priv, std::string& pub) {
    EVP_PKEY* key = gen_key();
    if (!key) return false;
    bool ok = raw_priv(key, priv) && raw_pub(key, pub);
    EVP_PKEY_free(key);
    return ok;
}

// 封信：临时公钥做 ECDH，随机盐做 HKDF，再交给 aes_encrypt。盐和临时公钥放在密文前面，对方才能拆。
bool e2e_seal(const std::string& recipient_pub, const std::string& plain, std::string& blob) {
    EVP_PKEY* eph = gen_key();
    EVP_PKEY* peer = load_pub(recipient_pub);
    if (!eph || !peer) {
        EVP_PKEY_free(eph);
        EVP_PKEY_free(peer);
        return false;
    }
    std::string epub, secret, salt(kSalt, '\0'), key;
    bool ok = raw_pub(eph, epub) && ecdh(eph, peer, secret);
    ok = ok && RAND_bytes(reinterpret_cast<unsigned char*>(salt.data()), kSalt) == 1;
    ok = ok && hkdf_key(secret, salt, key);
    EVP_PKEY_free(eph);
    EVP_PKEY_free(peer);
    if (!ok) return false;

    vecuc k(key.begin(), key.end());
    std::string aes;
    try {
        aes = aes_encrypt(k, plain);
    } catch (...) {
        return false;
    }
    blob = epub + salt + aes;
    return true;
}

// 拆信：取出临时公钥和盐，用自己的长期私钥做同样的 ECDH + HKDF，再 aes_decrypt
bool e2e_open(const std::string& priv, const std::string& blob, std::string& plain) {
    if (blob.size() < static_cast<std::size_t>(kPub + kSalt + 28)) return false;
    std::string epub = blob.substr(0, kPub);
    std::string salt = blob.substr(kPub, kSalt);
    std::string aes = blob.substr(kPub + kSalt);

    EVP_PKEY* mine = load_priv(priv);
    EVP_PKEY* peer = load_pub(epub);
    if (!mine || !peer) {
        EVP_PKEY_free(mine);
        EVP_PKEY_free(peer);
        return false;
    }
    std::string secret, key;
    bool ok = ecdh(mine, peer, secret) && hkdf_key(secret, salt, key);
    EVP_PKEY_free(mine);
    EVP_PKEY_free(peer);
    if (!ok) return false;

    vecuc k(key.begin(), key.end());
    try {
        plain = aes_decrypt(k, aes);
    } catch (...) {
        return false;
    }
    return true;
}
