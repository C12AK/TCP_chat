#include "store.h"

#include <QDir>

// 关掉本机库。
Store::~Store() {
    if (db) sqlite3_close(db);
}

// 打开或创建该用户目录下的 chat.db，并保证三张表存在。
bool Store::open(const QString& dir) {
    QDir().mkpath(dir);
    QString path = dir + "/chat.db";
    if (sqlite3_open(path.toUtf8().constData(), &db) != SQLITE_OK) return false;
    sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS msgs (msg_id INTEGER PRIMARY KEY, conv_id INTEGER, sender_id INTEGER, ts INTEGER, body TEXT, status INTEGER)", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS hidden (msg_id INTEGER PRIMARY KEY)", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS pending (nonce INTEGER PRIMARY KEY, conv_id INTEGER, body TEXT)", nullptr, nullptr, nullptr);
    return true;
}

// 记下一条已发出、还没拿到服务器消息 id 的明文。
void Store::addPending(quint64 convId, quint64 nonce, const QString& body) {
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO pending(nonce, conv_id, body) VALUES(?,?,?)", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(nonce));
    sqlite3_bind_int64(st, 2, static_cast<long long>(convId));
    QByteArray b = body.toUtf8();
    sqlite3_bind_text(st, 3, b.constData(), b.size(), SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

// ChatAck 到达：按 nonce 找到草稿，写成正式消息，再删掉草稿。
void Store::bindAck(quint64 nonce, quint64 msgId) {
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "SELECT conv_id, body FROM pending WHERE nonce=?", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(nonce));
    if (sqlite3_step(st) == SQLITE_ROW) {
        quint64 conv = static_cast<quint64>(sqlite3_column_int64(st, 0));
        QString body = QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(st, 1)));
        sqlite3_finalize(st);
        if (!hasMessage(msgId)) upsert(msgId, conv, 0, 0, body); // sender 0 在界面上显示成「我」
        sqlite3_prepare_v2(db, "DELETE FROM pending WHERE nonce=?", -1, &st, nullptr);
        sqlite3_bind_int64(st, 1, static_cast<long long>(nonce));
        sqlite3_step(st);
        sqlite3_finalize(st);
        return;
    }
    sqlite3_finalize(st);
}

// 本机是否已经有这个服务器消息 id。Push 和补拉可能各来一次，用来去重。
bool Store::hasMessage(quint64 msgId) const {
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "SELECT 1 FROM msgs WHERE msg_id=?", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(msgId));
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_ROW;
}

// 插入一条已解开的正文。主键冲突则忽略，避免后到的副本盖住已有内容。
void Store::upsert(quint64 msgId, quint64 convId, quint64 senderId, quint64 ts, const QString& body) {
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO msgs(msg_id, conv_id, sender_id, ts, body, status) VALUES(?,?,?,?,?,1)", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(msgId));
    sqlite3_bind_int64(st, 2, static_cast<long long>(convId));
    sqlite3_bind_int64(st, 3, static_cast<long long>(senderId));
    sqlite3_bind_int64(st, 4, static_cast<long long>(ts));
    QByteArray b = body.toUtf8();
    sqlite3_bind_text(st, 5, b.constData(), b.size(), SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

// 把状态往上升。后到的「送达」不会把已经写成「已读」的行盖回去。
void Store::setStatus(quint64 msgId, int status) {
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "UPDATE msgs SET status=? WHERE msg_id=? AND status<?", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, status);
    sqlite3_bind_int64(st, 2, static_cast<long long>(msgId));
    sqlite3_bind_int64(st, 3, status);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

// 本机不再显示这条。服务器上的密文还在，msgs 里的行也还在，所以补拉不会把它画回来。
void Store::hide(quint64 msgId) {
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO hidden(msg_id) VALUES(?)", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(msgId));
    sqlite3_step(st);
    sqlite3_finalize(st);
}

// 读出一个会话的可见正文，再加上还在发送中的草稿。needle 非空时只留整段包含它的行。
QVector<LocalMsg> Store::load(quint64 convId, const QString& needle) const {
    QVector<LocalMsg> out;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db,
                       "SELECT msg_id, sender_id, ts, body, status FROM msgs WHERE conv_id=? AND msg_id NOT IN (SELECT msg_id FROM hidden) ORDER BY msg_id",
                       -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(convId));
    while (sqlite3_step(st) == SQLITE_ROW) {
        LocalMsg m;
        m.msgId = static_cast<quint64>(sqlite3_column_int64(st, 0));
        m.convId = convId;
        m.senderId = static_cast<quint64>(sqlite3_column_int64(st, 1));
        m.ts = static_cast<quint64>(sqlite3_column_int64(st, 2));
        m.body = QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(st, 3)));
        m.status = static_cast<int>(sqlite3_column_int64(st, 4));
        if (!needle.isEmpty() && !m.body.contains(needle)) continue; // 区分大小写的子串，不是模糊搜索
        out.push_back(m);
    }
    sqlite3_finalize(st);

    sqlite3_prepare_v2(db, "SELECT nonce, body FROM pending WHERE conv_id=?", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<long long>(convId));
    while (sqlite3_step(st) == SQLITE_ROW) {
        LocalMsg m;
        m.pending = true;
        m.nonce = static_cast<quint64>(sqlite3_column_int64(st, 0));
        m.convId = convId;
        m.body = QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(st, 1)));
        if (!needle.isEmpty() && !m.body.contains(needle)) continue; // 区分大小写的子串，不是模糊搜索
        out.push_back(m);
    }
    sqlite3_finalize(st);
    return out;
}

// 本机已有正文里最大的服务器消息 id。没有行时是 0，上线补拉从这里往后要。
quint64 Store::maxId() const {
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "SELECT COALESCE(MAX(msg_id),0) FROM msgs", -1, &st, nullptr);
    quint64 id = 0;
    if (sqlite3_step(st) == SQLITE_ROW) id = static_cast<quint64>(sqlite3_column_int64(st, 0));
    sqlite3_finalize(st);
    return id;
}
