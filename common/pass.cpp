#include "pass.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace {

constexpr int kIters = 100000;  // 故意做慢，拿到库也难快速试口令
constexpr int kSalt = 16;
constexpr int kHash = 32;

// 口令不入库。盐是随机的，哈希是 PBKDF2-HMAC-SHA256，10 万次
bool pbkdf2(const std::string& password, const std::string& salt, std::string& hash) {
    hash.assign(kHash, '\0');
    return PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                             reinterpret_cast<const unsigned char*>(salt.data()),
                             static_cast<int>(salt.size()),
                             kIters, EVP_sha256(), kHash,
                             reinterpret_cast<unsigned char*>(hash.data())) == 1;
}

}  // namespace

// 生成随机盐并计算哈希。salt、hash 由本函数写入，口令本身不保留。
bool make_pass_hash(const std::string& password, std::string& salt, std::string& hash) {
    salt.assign(kSalt, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(salt.data()), kSalt) != 1) return false;
    return pbkdf2(password, salt, hash);
}

// 用保存的盐重算哈希再比较。盐或哈希长度不对、口令错，都返回 false。
bool check_pass_hash(const std::string& password, const std::string& salt, const std::string& hash) {
    if (salt.size() != kSalt || hash.size() != kHash) return false;
    std::string got;
    if (!pbkdf2(password, salt, got)) return false;
    // 不在第一个不同字节就返回，避免用比较耗时猜中前面有几字节相同
    return CRYPTO_memcmp(got.data(), hash.data(), kHash) == 0;
}
