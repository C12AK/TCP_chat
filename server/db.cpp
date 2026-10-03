#include "db.h"

#include "pass.h"
#include "sqlite3_min.h"

#include <chrono>
#include <filesystem>
#include <stdexcept>

namespace {

// 墙上时钟的毫秒数，写进用户创建时间和消息时间戳。
int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// 读文本列。NULL 当成空串。指针在下次 step 前有效，所以立刻拷走。
std::string col_text(sqlite3_stmt* st, int i) {
    auto* p = sqlite3_column_text(st, i);
    if (!p) return {};
    return reinterpret_cast<const char*>(p);
}

// 读 BLOB 列。同样立刻拷进 string，不能留着 SQLite 的内部指针。
std::string col_blob(sqlite3_stmt* st, int i) {
    const void* p = sqlite3_column_blob(st, i);
    int n = sqlite3_column_bytes(st, i);
    if (!p || n <= 0) return {};
    return std::string(static_cast<const char*>(p), static_cast<std::size_t>(n));
}

}  // namespace

// 打开或创建库文件，建好用户、会话、消息、好友、群这几张表，并写入 id=1 的系统用户
Database::Database(const std::string& path) {
    std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
        throw std::runtime_error("无法打开数据库");
    }

    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA synchronous=NORMAL;");
    exec("PRAGMA busy_timeout=5000;");

    exec(
        "CREATE TABLE IF NOT EXISTS users ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " username TEXT UNIQUE NOT NULL,"
        " pass_salt BLOB NOT NULL,"
        " pass_hash BLOB NOT NULL,"
        " pubkey BLOB NOT NULL,"
        " created_ms INTEGER NOT NULL);");
    exec(
        "CREATE TABLE IF NOT EXISTS conversations ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " kind INTEGER NOT NULL,"
        " title TEXT NOT NULL,"
        " owner_id INTEGER NOT NULL,"
        " alive INTEGER NOT NULL DEFAULT 1);");
    exec(
        "CREATE TABLE IF NOT EXISTS conv_members ("
        " conv_id INTEGER NOT NULL,"
        " user_id INTEGER NOT NULL,"
        " PRIMARY KEY(conv_id, user_id));");
    exec(
        "CREATE TABLE IF NOT EXISTS messages ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " conv_id INTEGER NOT NULL,"
        " sender_id INTEGER NOT NULL,"
        " client_nonce INTEGER NOT NULL,"
        " ts_ms INTEGER NOT NULL,"
        " UNIQUE(sender_id, client_nonce));");
    exec(
        "CREATE TABLE IF NOT EXISTS message_copies ("
        " msg_id INTEGER NOT NULL,"
        " recipient_id INTEGER NOT NULL,"
        " ciphertext BLOB NOT NULL,"
        " PRIMARY KEY(msg_id, recipient_id));");
    exec("CREATE INDEX IF NOT EXISTS idx_copies_user ON message_copies(recipient_id, msg_id);");
    exec(
        "CREATE TABLE IF NOT EXISTS read_cursors ("
        " user_id INTEGER NOT NULL,"
        " conv_id INTEGER NOT NULL,"
        " last_read_id INTEGER NOT NULL,"
        " PRIMARY KEY(user_id, conv_id));");
    exec(
        "CREATE TABLE IF NOT EXISTS friend_requests ("
        " from_id INTEGER NOT NULL,"
        " to_id INTEGER NOT NULL,"
        " PRIMARY KEY(from_id, to_id));");
    exec(
        "CREATE TABLE IF NOT EXISTS friends ("
        " a INTEGER NOT NULL,"
        " b INTEGER NOT NULL,"
        " conv_id INTEGER NOT NULL,"
        " PRIMARY KEY(a, b));");

    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "SELECT id FROM users WHERE id=1", -1, &st, nullptr);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_ROW) {
        std::string salt, hash;
        std::string secret(32, '\x5a');
        if (!make_pass_hash(secret, salt, hash)) throw std::runtime_error("系统账号初始化失败");
        sqlite3_prepare_v2(db,
                           "INSERT INTO users(id, username, pass_salt, pass_hash, pubkey, created_ms)"
                           " VALUES(1, '系统', ?, ?, ?, ?)",
                           -1, &st, nullptr);
        std::string pub(32, '\0');
        sqlite3_bind_blob(st, 1, salt.data(), static_cast<int>(salt.size()), SQLITE_TRANSIENT);
        sqlite3_bind_blob(st, 2, hash.data(), static_cast<int>(hash.size()), SQLITE_TRANSIENT);
        sqlite3_bind_blob(st, 3, pub.data(), 32, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 4, now_ms());
        if (sqlite3_step(st) != SQLITE_DONE) {
            sqlite3_finalize(st);
            throw std::runtime_error("系统账号初始化失败");
        }
        sqlite3_finalize(st);
    }
}

// 关闭库文件。
Database::~Database() {
    if (db) sqlite3_close(db);
}

// 执行不带参数的 SQL。错误串由 SQLite 分配，用完必须 free。
bool Database::exec(const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK;
}

// 校验用户名、口令和公钥，写入 users，并给这个用户建一条系统会话。失败原因放进 err。
bool Database::register_user(const std::string& username, const std::string& password,
                             const std::string& pubkey, uint64_t& out_id, std::string& err) {
    std::lock_guard lock(mu);
    if (username.empty() || username.size() > 500 || username == "系统") {
        err = "用户名不合法";
        return false;
    }
    if (password.empty() || password.size() > 128) {
        err = "密码不合法";
        return false;
    }
    if (pubkey.size() != 32) {
        err = "公钥长度不对";
        return false;
    }

    std::string salt, hash;
    if (!make_pass_hash(password, salt, hash)) {
        err = "口令处理失败";
        return false;
    }

    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db,
                       "INSERT INTO users(username, pass_salt, pass_hash, pubkey, created_ms)"
                       " VALUES(?, ?, ?, ?, ?)",
                       -1, &st, nullptr);
    sqlite3_bind_text(st, 1, username.data(), static_cast<int>(username.size()), SQLITE_TRANSIENT);
    sqlite3_bind_blob(st, 2, salt.data(), static_cast<int>(salt.size()), SQLITE_TRANSIENT);
    sqlite3_bind_blob(st, 3, hash.data(), static_cast<int>(hash.size()), SQLITE_TRANSIENT);
    sqlite3_bind_blob(st, 4, pubkey.data(), static_cast<int>(pubkey.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, now_ms());
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc == SQLITE_CONSTRAINT) {
        err = "用户名已被占用"; // username 有 UNIQUE
        return false;
    }
    if (rc != SQLITE_DONE) {
        err = "注册失败";
        return false;
    }
    out_id = static_cast<uint64_t>(sqlite3_last_insert_rowid(db));
    ensure_system_conv(out_id);
    return true;
}

// 按用户名取出盐和哈希并核对。系统账号一律失败，且不区分「没有此人」和「口令错」。
bool Database::verify_login(const std::string& username, const std::string& password,
                            User& out, std::string& err) {
    std::lock_guard lock(mu);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "SELECT id, username, pass_salt, pass_hash, pubkey FROM users WHERE username=?",
                       -1, &st, nullptr);
    sqlite3_bind_text(st, 1, username.data(), static_cast<int>(username.size()), SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(st);
        err = "用户名或密码错误";
        return false;
    }
    out.id = static_cast<uint64_t>(sqlite3_column_int64(st, 0));
    out.username = col_text(st, 1);
    std::string salt = col_blob(st, 2);
    std::string hash = col_blob(st, 3);
    out.pubkey = col_blob(st, 4);
    sqlite3_finalize(st);
    if (out.id == kSystemId || !check_pass_hash(password, salt, hash)) {
        err = "用户名或密码错误";
        return false;
    }
    return true;
}

// 按 id 取用户名和公钥。找不到返回 false。
bool Database::user_by_id(uint64_t id, User& out) {
    std::lock_guard lock(mu);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "SELECT id, username, pubkey FROM users WHERE id=?", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(id));
    int rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(st);
        return false;
    }
    out.id = static_cast<uint64_t>(sqlite3_column_int64(st, 0));
    out.username = col_text(st, 1);
    out.pubkey = col_blob(st, 2);
    sqlite3_finalize(st);
    return true;
}

// 按完整用户名取 id 和公钥。找不到返回 false。
bool Database::user_by_name(const std::string& username, User& out) {
    std::lock_guard lock(mu);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "SELECT id, username, pubkey FROM users WHERE username=?", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, username.data(), static_cast<int>(username.size()), SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(st);
        return false;
    }
    out.id = static_cast<uint64_t>(sqlite3_column_int64(st, 0));
    out.username = col_text(st, 1);
    out.pubkey = col_blob(st, 2);
    sqlite3_finalize(st);
    return true;
}

// 找到或创建该用户和「系统」的会话。调用方已经持有 mu，这里不再加锁。
uint64_t Database::ensure_system_conv(uint64_t user_id) {
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db,
                       "SELECT c.id FROM conversations c JOIN conv_members m ON m.conv_id=c.id"
                       " WHERE c.kind=2 AND m.user_id=?",
                       -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(user_id));
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        uint64_t id = static_cast<uint64_t>(sqlite3_column_int64(st, 0));
        sqlite3_finalize(st);
        return id;
    }
    sqlite3_finalize(st);

    sqlite3_prepare_v2(db, "INSERT INTO conversations(kind, title, owner_id, alive) VALUES(2, '系统', 1, 1)",
                       -1, &st, nullptr);
    sqlite3_step(st);
    sqlite3_finalize(st);
    uint64_t conv = static_cast<uint64_t>(sqlite3_last_insert_rowid(db));

    sqlite3_prepare_v2(db, "INSERT INTO conv_members(conv_id, user_id) VALUES(?, ?)", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(conv));
    sqlite3_bind_int64(st, 2, static_cast<long long>(kSystemId));
    sqlite3_step(st);
    sqlite3_reset(st);
    sqlite3_bind_int64(st, 1, static_cast<long long>(conv));
    sqlite3_bind_int64(st, 2, static_cast<long long>(user_id));
    sqlite3_step(st);
    sqlite3_finalize(st);
    return conv;
}

// 对外入口：加锁后交给 ensure_system_conv。
uint64_t Database::system_conv(uint64_t user_id) {
    std::lock_guard lock(mu);
    return ensure_system_conv(user_id);
}

// 列出这个用户还在里面、且会话仍然有效的那些会话，并填上已读位置和最新消息 id。
std::vector<Database::Conv> Database::list_convs(uint64_t user_id) {
    std::lock_guard lock(mu);
    std::vector<Conv> out;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db,
                       "SELECT c.id, c.kind, c.title, c.owner_id FROM conversations c"
                       " JOIN conv_members m ON m.conv_id=c.id"
                       " WHERE m.user_id=? AND c.alive=1 ORDER BY c.id",
                       -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(user_id));
    while (sqlite3_step(st) == SQLITE_ROW) {
        Conv c;
        c.id = static_cast<uint64_t>(sqlite3_column_int64(st, 0));
        c.kind = static_cast<int>(sqlite3_column_int64(st, 1));
        c.title = col_text(st, 2);
        c.owner_id = static_cast<uint64_t>(sqlite3_column_int64(st, 3));
        out.push_back(c);
    }
    sqlite3_finalize(st);

    for (auto& c : out) {
        if (c.kind == 0) { // 私聊标题换成对方用户名，建会话时标题是空的
            sqlite3_prepare_v2(db,
                               "SELECT u.username FROM conv_members m JOIN users u ON u.id=m.user_id"
                               " WHERE m.conv_id=? AND m.user_id<>?",
                               -1, &st, nullptr);
            sqlite3_bind_int64(st, 1, static_cast<long long>(c.id));
            sqlite3_bind_int64(st, 2, static_cast<long long>(user_id));
            if (sqlite3_step(st) == SQLITE_ROW) c.title = col_text(st, 0);
            sqlite3_finalize(st);
        }

        sqlite3_prepare_v2(db, "SELECT last_read_id FROM read_cursors WHERE user_id=? AND conv_id=?",
                           -1, &st, nullptr);
        sqlite3_bind_int64(st, 1, static_cast<long long>(user_id));
        sqlite3_bind_int64(st, 2, static_cast<long long>(c.id));
        if (sqlite3_step(st) == SQLITE_ROW) c.last_read_id = static_cast<uint64_t>(sqlite3_column_int64(st, 0));
        sqlite3_finalize(st);

        sqlite3_prepare_v2(db, "SELECT COALESCE(MAX(id),0) FROM messages WHERE conv_id=?", -1, &st, nullptr);
        sqlite3_bind_int64(st, 1, static_cast<long long>(c.id));
        if (sqlite3_step(st) == SQLITE_ROW) c.latest_id = static_cast<uint64_t>(sqlite3_column_int64(st, 0));
        sqlite3_finalize(st);
    }
    return out;
}

// 读取会话种类和群主。会话不存在或已经关闭（alive=0）时返回 false。
bool Database::conv_kind(uint64_t conv_id, int& kind, uint64_t& owner_id) {
    std::lock_guard lock(mu);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "SELECT kind, owner_id, alive FROM conversations WHERE id=?", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(conv_id));
    int rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(st);
        return false;
    }
    kind = static_cast<int>(sqlite3_column_int64(st, 0));
    owner_id = static_cast<uint64_t>(sqlite3_column_int64(st, 1));
    int alive = static_cast<int>(sqlite3_column_int64(st, 2));
    sqlite3_finalize(st);
    return alive == 1;
}

// 这个用户现在是否还在该会话的成员表里。
bool Database::is_member(uint64_t conv_id, uint64_t user_id) {
    std::lock_guard lock(mu);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "SELECT 1 FROM conv_members WHERE conv_id=? AND user_id=?", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(conv_id));
    sqlite3_bind_int64(st, 2, static_cast<long long>(user_id));
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_ROW;
}

// 当前成员 id 列表。退群或解散之后，离开的人不会出现在这里。
std::vector<uint64_t> Database::members(uint64_t conv_id) {
    std::lock_guard lock(mu);
    std::vector<uint64_t> ids;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "SELECT user_id FROM conv_members WHERE conv_id=?", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(conv_id));
    while (sqlite3_step(st) == SQLITE_ROW) ids.push_back(static_cast<uint64_t>(sqlite3_column_int64(st, 0)));
    sqlite3_finalize(st);
    return ids;
}

// 写入一条消息的多份密文。同一发送者的 nonce 已存在则返回旧 id，并把 dup 设为 true。
bool Database::insert_message(uint64_t conv_id, uint64_t sender_id, uint64_t nonce, uint64_t ts_ms,
                              const std::vector<MemberSeal>& copies, uint64_t& msg_id, bool& dup,
                              std::string& err) {
    std::lock_guard lock(mu);
    dup = false;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "SELECT id FROM messages WHERE sender_id=? AND client_nonce=?", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(sender_id));
    sqlite3_bind_int64(st, 2, static_cast<long long>(nonce));
    if (sqlite3_step(st) == SQLITE_ROW) {
        msg_id = static_cast<uint64_t>(sqlite3_column_int64(st, 0));
        sqlite3_finalize(st);
        dup = true;
        return true;
    }
    sqlite3_finalize(st);

    exec("BEGIN"); // 消息行和每一份密文要一起成功，中途失败就整笔回滚
    sqlite3_prepare_v2(db,
                       "INSERT INTO messages(conv_id, sender_id, client_nonce, ts_ms) VALUES(?, ?, ?, ?)",
                       -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(conv_id));
    sqlite3_bind_int64(st, 2, static_cast<long long>(sender_id));
    sqlite3_bind_int64(st, 3, static_cast<long long>(nonce));
    sqlite3_bind_int64(st, 4, static_cast<long long>(ts_ms));
    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        exec("ROLLBACK");
        err = "消息写入失败";
        return false;
    }
    sqlite3_finalize(st);
    msg_id = static_cast<uint64_t>(sqlite3_last_insert_rowid(db));

    sqlite3_prepare_v2(db, "INSERT INTO message_copies(msg_id, recipient_id, ciphertext) VALUES(?, ?, ?)",
                       -1, &st, nullptr);
    for (const auto& c : copies) {
        sqlite3_reset(st);
        sqlite3_bind_int64(st, 1, static_cast<long long>(msg_id));
        sqlite3_bind_int64(st, 2, static_cast<long long>(c.user_id));
        sqlite3_bind_blob(st, 3, c.ciphertext.data(), static_cast<int>(c.ciphertext.size()), SQLITE_TRANSIENT);
        if (sqlite3_step(st) != SQLITE_DONE) {
            sqlite3_finalize(st);
            exec("ROLLBACK");
            err = "消息写入失败";
            return false;
        }
    }
    sqlite3_finalize(st);
    exec("COMMIT");
    return true;
}

// 该用户 id 大于 after_id 的密文，按消息 id 升序，最多 limit 条。不看会话是否还活着。
std::vector<Database::Copy> Database::sync_for(uint64_t user_id, uint64_t after_id, int limit) {
    std::lock_guard lock(mu);
    std::vector<Copy> out;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db,
                       "SELECT m.id, m.conv_id, m.sender_id, m.ts_ms, c.ciphertext"
                       " FROM message_copies c JOIN messages m ON m.id=c.msg_id"
                       " WHERE c.recipient_id=? AND m.id>? ORDER BY m.id LIMIT ?",
                       -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(user_id));
    sqlite3_bind_int64(st, 2, static_cast<long long>(after_id));
    sqlite3_bind_int64(st, 3, limit);
    while (sqlite3_step(st) == SQLITE_ROW) {
        Copy c;
        c.msg_id = static_cast<uint64_t>(sqlite3_column_int64(st, 0));
        c.conv_id = static_cast<uint64_t>(sqlite3_column_int64(st, 1));
        c.sender_id = static_cast<uint64_t>(sqlite3_column_int64(st, 2));
        c.ts_ms = static_cast<uint64_t>(sqlite3_column_int64(st, 3));
        c.ciphertext = col_blob(st, 4);
        out.push_back(std::move(c));
    }
    sqlite3_finalize(st);
    return out;
}

// 查出这条消息属于哪个会话、是谁发的。回执要转给原发送者时用。
bool Database::message_info(uint64_t msg_id, uint64_t& conv_id, uint64_t& sender_id) {
    std::lock_guard lock(mu);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "SELECT conv_id, sender_id FROM messages WHERE id=?", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(msg_id));
    int rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(st);
        return false;
    }
    conv_id = static_cast<uint64_t>(sqlite3_column_int64(st, 0));
    sender_id = static_cast<uint64_t>(sqlite3_column_int64(st, 1));
    sqlite3_finalize(st);
    return true;
}

// 把已读位置推进到 msg_id。已有更大的游标时保持不动，旧回执不能把它拉回去。
bool Database::set_read(uint64_t user_id, uint64_t conv_id, uint64_t msg_id) {
    std::lock_guard lock(mu);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db,
                       "INSERT INTO read_cursors(user_id, conv_id, last_read_id) VALUES(?, ?, ?)"
                       " ON CONFLICT(user_id, conv_id) DO UPDATE SET"
                       " last_read_id=MAX(last_read_id, excluded.last_read_id)",
                       -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(user_id));
    sqlite3_bind_int64(st, 2, static_cast<long long>(conv_id));
    sqlite3_bind_int64(st, 3, static_cast<long long>(msg_id));
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

// 按完整用户名查人，并把和自己的关系写入 relation：0 无，1 已申请，2 待我处理，3 好友，4 自己。
// 内部还会调用同样加锁的查询，所以 mu 用的是递归锁。
bool Database::search_user(uint64_t self, const std::string& username, User& out, int& relation) {
    std::lock_guard lock(mu);
    if (!user_by_name(username, out)) return false;
    if (out.id == self) {
        relation = 4;
        return true;
    }
    uint64_t a = std::min(self, out.id); // 好友行只存 a < b，谁先申请都是同一行
    uint64_t b = std::max(self, out.id);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "SELECT 1 FROM friends WHERE a=? AND b=?", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(a));
    sqlite3_bind_int64(st, 2, static_cast<long long>(b));
    if (sqlite3_step(st) == SQLITE_ROW) {
        sqlite3_finalize(st);
        relation = 3;
        return true;
    }
    sqlite3_finalize(st);

    sqlite3_prepare_v2(db, "SELECT 1 FROM friend_requests WHERE from_id=? AND to_id=?", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(self));
    sqlite3_bind_int64(st, 2, static_cast<long long>(out.id));
    if (sqlite3_step(st) == SQLITE_ROW) {
        sqlite3_finalize(st);
        relation = 1;
        return true;
    }
    sqlite3_finalize(st);

    sqlite3_prepare_v2(db, "SELECT 1 FROM friend_requests WHERE from_id=? AND to_id=?", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(out.id));
    sqlite3_bind_int64(st, 2, static_cast<long long>(self));
    if (sqlite3_step(st) == SQLITE_ROW) {
        sqlite3_finalize(st);
        relation = 2;
        return true;
    }
    sqlite3_finalize(st);
    relation = 0;
    return true;
}

// 记下 from 向 to 发出的申请。已是好友、已经申请过、或目标是自己和系统，都失败。
bool Database::add_request(uint64_t from, uint64_t to, std::string& err) {
    std::lock_guard lock(mu);
    if (from == to || to == kSystemId || from == kSystemId) {
        err = "不能添加该用户";
        return false;
    }
    User u;
    if (!user_by_id(to, u)) {
        err = "没有这个用户";
        return false;
    }
    int rel = 0;
    User found;
    search_user(from, u.username, found, rel);
    if (rel == 3) {
        err = "已经是好友";
        return false;
    }
    if (rel == 1) {
        err = "已发送请求";
        return false;
    }
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "INSERT INTO friend_requests(from_id, to_id) VALUES(?, ?)", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(from));
    sqlite3_bind_int64(st, 2, static_cast<long long>(to));
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        err = "申请失败";
        return false;
    }
    return true;
}

// 处理 requester 发给 self 的申请。同意时建立私聊并把会话 id 写入 conv_id；拒绝只删申请。
bool Database::respond_request(uint64_t self, uint64_t requester, bool accept, uint64_t& conv_id,
                               std::string& err) {
    std::lock_guard lock(mu);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "DELETE FROM friend_requests WHERE from_id=? AND to_id=?", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(requester));
    sqlite3_bind_int64(st, 2, static_cast<long long>(self));
    sqlite3_step(st);
    int changed = 0;
    sqlite3_finalize(st);

    // changes() 未声明，用再查一次是否还在来判断。上面已删，用 rowcount 近似：再 SELECT
    // changes() 是本连接最近一次增删改动了几行。为 0 表示根本没有这条申请
    sqlite3_prepare_v2(db, "SELECT changes()", -1, &st, nullptr);
    if (sqlite3_step(st) == SQLITE_ROW) changed = static_cast<int>(sqlite3_column_int64(st, 0));
    sqlite3_finalize(st);
    if (changed == 0) {
        err = "没有这条申请";
        return false;
    }
    if (!accept) {
        conv_id = 0;
        return true;
    }

    sqlite3_prepare_v2(db, "DELETE FROM friend_requests WHERE from_id=? AND to_id=?", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(self));
    sqlite3_bind_int64(st, 2, static_cast<long long>(requester));
    sqlite3_step(st);
    sqlite3_finalize(st);

    uint64_t a = std::min(self, requester);
    uint64_t b = std::max(self, requester);
    sqlite3_prepare_v2(db, "SELECT conv_id FROM friends WHERE a=? AND b=?", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(a));
    sqlite3_bind_int64(st, 2, static_cast<long long>(b));
    if (sqlite3_step(st) == SQLITE_ROW) {
        conv_id = static_cast<uint64_t>(sqlite3_column_int64(st, 0));
        sqlite3_finalize(st);
        return true;
    }
    sqlite3_finalize(st);

    sqlite3_prepare_v2(db, "INSERT INTO conversations(kind, title, owner_id, alive) VALUES(0, '', 0, 1)",
                       -1, &st, nullptr);
    sqlite3_step(st);
    sqlite3_finalize(st);
    conv_id = static_cast<uint64_t>(sqlite3_last_insert_rowid(db));

    sqlite3_prepare_v2(db, "INSERT INTO conv_members(conv_id, user_id) VALUES(?, ?)", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(conv_id));
    sqlite3_bind_int64(st, 2, static_cast<long long>(self));
    sqlite3_step(st);
    sqlite3_reset(st);
    sqlite3_bind_int64(st, 1, static_cast<long long>(conv_id));
    sqlite3_bind_int64(st, 2, static_cast<long long>(requester));
    sqlite3_step(st);
    sqlite3_finalize(st);

    sqlite3_prepare_v2(db, "INSERT INTO friends(a, b, conv_id) VALUES(?, ?, ?)", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(a));
    sqlite3_bind_int64(st, 2, static_cast<long long>(b));
    sqlite3_bind_int64(st, 3, static_cast<long long>(conv_id));
    sqlite3_step(st);
    sqlite3_finalize(st);
    return true;
}

// 解除好友：删掉好友行和成员，会话标成关闭。历史密文留在 message_copies 里。
bool Database::delete_friend(uint64_t a_id, uint64_t b_id, uint64_t& conv_id) {
    std::lock_guard lock(mu);
    uint64_t a = std::min(a_id, b_id);
    uint64_t b = std::max(a_id, b_id);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "SELECT conv_id FROM friends WHERE a=? AND b=?", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(a));
    sqlite3_bind_int64(st, 2, static_cast<long long>(b));
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        return false;
    }
    conv_id = static_cast<uint64_t>(sqlite3_column_int64(st, 0));
    sqlite3_finalize(st);

    sqlite3_prepare_v2(db, "DELETE FROM friends WHERE a=? AND b=?", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(a));
    sqlite3_bind_int64(st, 2, static_cast<long long>(b));
    sqlite3_step(st);
    sqlite3_finalize(st);

    sqlite3_prepare_v2(db, "DELETE FROM conv_members WHERE conv_id=?", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(conv_id));
    sqlite3_step(st);
    sqlite3_finalize(st);

    sqlite3_prepare_v2(db, "UPDATE conversations SET alive=0 WHERE id=?", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(conv_id)); // 列表只返回 alive=1
    sqlite3_step(st);
    sqlite3_finalize(st);
    return true;
}

// 建群。创建者同时是群主和唯一成员。群名不能空，也不能超过 64 字节。
bool Database::create_group(uint64_t owner, const std::string& title, uint64_t& conv_id, std::string& err) {
    std::lock_guard lock(mu);
    if (title.empty() || title.size() > 64) {
        err = "群名不合法";
        return false;
    }
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "INSERT INTO conversations(kind, title, owner_id, alive) VALUES(1, ?, ?, 1)",
                       -1, &st, nullptr);
    sqlite3_bind_text(st, 1, title.data(), static_cast<int>(title.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, static_cast<long long>(owner));
    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        err = "建群失败";
        return false;
    }
    sqlite3_finalize(st);
    conv_id = static_cast<uint64_t>(sqlite3_last_insert_rowid(db));
    sqlite3_prepare_v2(db, "INSERT INTO conv_members(conv_id, user_id) VALUES(?, ?)", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(conv_id));
    sqlite3_bind_int64(st, 2, static_cast<long long>(owner));
    sqlite3_step(st);
    sqlite3_finalize(st);
    return true;
}

// 把 user_id 拉进群。by 必须已在群里，且两人是好友，群人数还不到 50。
bool Database::invite_group(uint64_t conv_id, uint64_t by, uint64_t user_id, std::string& err) {
    std::lock_guard lock(mu);
    int kind = 0;
    uint64_t owner = 0;
    if (!conv_kind(conv_id, kind, owner) || kind != 1) {
        err = "群不存在";
        return false;
    }
    if (!is_member(conv_id, by)) {
        err = "你不在这个群里";
        return false;
    }
    if (user_id == kSystemId) {
        err = "不能邀请该用户";
        return false;
    }
    if (is_member(conv_id, user_id)) {
        err = "已经在群里";
        return false;
    }
    uint64_t a = std::min(by, user_id);
    uint64_t b = std::max(by, user_id);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "SELECT 1 FROM friends WHERE a=? AND b=?", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(a));
    sqlite3_bind_int64(st, 2, static_cast<long long>(b));
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        err = "只能邀请好友";
        return false;
    }
    sqlite3_finalize(st);

    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM conv_members WHERE conv_id=?", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(conv_id));
    sqlite3_step(st);
    int n = static_cast<int>(sqlite3_column_int64(st, 0));
    sqlite3_finalize(st);
    if (n >= 50) { // 含邀请者在内，加完这人不能超过 50
        err = "群人数已满";
        return false;
    }

    sqlite3_prepare_v2(db, "INSERT INTO conv_members(conv_id, user_id) VALUES(?, ?)", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(conv_id));
    sqlite3_bind_int64(st, 2, static_cast<long long>(user_id));
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        err = "邀请失败";
        return false;
    }
    return true;
}

// 普通成员退出。群主不能走这里，会话仍然有效，其他人的历史还在。
bool Database::leave_group(uint64_t conv_id, uint64_t user_id, std::string& err) {
    std::lock_guard lock(mu);
    int kind = 0;
    uint64_t owner = 0;
    if (!conv_kind(conv_id, kind, owner) || kind != 1) {
        err = "群不存在";
        return false;
    }
    if (user_id == owner) {
        err = "群主不能退出，只能解散";
        return false;
    }
    if (!is_member(conv_id, user_id)) {
        err = "你不在这个群里";
        return false;
    }
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "DELETE FROM conv_members WHERE conv_id=? AND user_id=?", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(conv_id));
    sqlite3_bind_int64(st, 2, static_cast<long long>(user_id));
    sqlite3_step(st);
    sqlite3_finalize(st);
    return true;
}

// 只有群主能解散。former 带回解散前的成员，调用方用它发系统说明。历史密文不删。
bool Database::dissolve_group(uint64_t conv_id, uint64_t owner, std::vector<uint64_t>& former, std::string& err) {
    std::lock_guard lock(mu);
    int kind = 0;
    uint64_t own = 0;
    if (!conv_kind(conv_id, kind, own) || kind != 1) {
        err = "群不存在";
        return false;
    }
    if (own != owner) {
        err = "只有群主能解散";
        return false;
    }
    former = members(conv_id); // 先抄名单，下面删掉成员后就查不到了
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "DELETE FROM conv_members WHERE conv_id=?", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(conv_id));
    sqlite3_step(st);
    sqlite3_finalize(st);
    sqlite3_prepare_v2(db, "UPDATE conversations SET alive=0 WHERE id=?", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(conv_id));
    sqlite3_step(st);
    sqlite3_finalize(st);
    return true;
}
