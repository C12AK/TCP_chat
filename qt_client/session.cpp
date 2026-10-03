#include "session.h"

#include "log.h"

#include <QPair>

#include <string>

namespace {

// R::u64 要 uint64_t 的引用。这套 Qt 里 quint64 是另一种 64 位类型，引用绑不上去。
bool take64(R& r, quint64& out) {
    uint64_t v = 0;
    if (!r.u64(v)) return false;
    out = v;
    return true;
}

// 协议用 std::string 装字节，窗口用 QByteArray。两边按原始字节拷贝，不做文本转码。
QByteArray toBytes(const std::string& s) { return QByteArray(s.data(), static_cast<int>(s.size())); }

std::string fromBytes(const QByteArray& b) { return std::string(b.constData(), static_cast<std::size_t>(b.size())); }

}  // namespace

// 把套接字的可读、断开、连上，以及 15 秒定时器，接到本对象的处理函数。
Session::Session(QObject* parent) : QObject(parent) {
    connect(&sock_, &QTcpSocket::readyRead, this, &Session::onReadyRead);
    connect(&sock_, &QTcpSocket::disconnected, this, &Session::onDisconnected);
    connect(&sock_, &QTcpSocket::connected, this, [this] { emit connected(); });
    heartbeat_.setInterval(15000);
    connect(&heartbeat_, &QTimer::timeout, this, &Session::sendHeartbeat);
}

// 连到服务器。若上一条连接还在，先掐掉，并清掉它留下的半包。
void Session::open(const QString& host, quint16 port) {
    if (sock_.state() != QAbstractSocket::UnconnectedState) sock_.abort();
    acc_.clear();
    sock_.connectToHost(host, port);
}

// 组帧后放进 Qt 的发送缓冲，由事件循环真正写到套接字。
void Session::sendFrame(MsgType type, const QByteArray& payload) {
    std::string frame = build_frame(type, fromBytes(payload));
    sock_.write(frame.data(), static_cast<qint64>(frame.size()));
}

// 发送注册帧：用户名、口令、32 字节公钥。
void Session::registerAccount(const QString& user, const QString& pass, const QByteArray& pub) {
    W w;
    w.str(user.toStdString());
    w.str(pass.toStdString());
    w.str(fromBytes(pub));
    sendFrame(MsgType::Register, toBytes(w.take()));
}

// 发送登录帧，并开始每 15 秒一次的空心跳。心跳不等登录成功。
void Session::login(const QString& user, const QString& pass) {
    W w;
    w.str(user.toStdString());
    w.str(pass.toStdString());
    sendFrame(MsgType::Login, toBytes(w.take()));
    wantHeartbeat_ = true;
    heartbeat_.start();
}

// 向服务器要 id 大于 afterId 的密文。
void Session::requestSync(quint64 afterId) {
    W w;
    w.u64(afterId);
    sendFrame(MsgType::SyncReq, toBytes(w.take()));
}

// 发送聊天。copies 是每个成员一份已经封好的密文。
void Session::sendChat(quint64 convId, quint64 nonce, const QVector<QPair<quint64, QByteArray>>& copies) {
    W w;
    w.u64(convId);
    w.u64(nonce);
    w.u64(static_cast<quint64>(copies.size()));
    for (const auto& c : copies) {
        w.u64(c.first);
        w.str(fromBytes(c.second));
    }
    sendFrame(MsgType::ChatSend, toBytes(w.take()));
}

// 发送回执。kind 1 是送达，2 是已读。
void Session::sendReceipt(quint8 kind, quint64 convId, quint64 msgId) {
    W w;
    w.u8(kind);
    w.u64(convId);
    w.u64(msgId);
    sendFrame(MsgType::Receipt, toBytes(w.take()));
}

// 按完整用户名查关系，不改服务器上的数据。
void Session::searchUser(const QString& name) {
    W w;
    w.str(name.toStdString());
    sendFrame(MsgType::FriendSearch, toBytes(w.take()));
}

// 向 id 发出好友申请。
void Session::requestFriend(quint64 id) {
    W w;
    w.u64(id);
    sendFrame(MsgType::FriendRequest, toBytes(w.take()));
}

// 同意或拒绝 requester 发来的申请。
void Session::respondFriend(quint64 requester, bool accept) {
    W w;
    w.u64(requester);
    w.u8(accept ? 1 : 0);
    sendFrame(MsgType::FriendRespond, toBytes(w.take()));
}

// 和 id 互相解除好友。
void Session::deleteFriend(quint64 id) {
    W w;
    w.u64(id);
    sendFrame(MsgType::FriendDelete, toBytes(w.take()));
}

// 用 title 建群，自己成为群主。
void Session::createGroup(const QString& title) {
    W w;
    w.str(title.toStdString());
    sendFrame(MsgType::GroupCreate, toBytes(w.take()));
}

// 把 userId 邀请进 convId 这个群。
void Session::inviteGroup(quint64 convId, quint64 userId) {
    W w;
    w.u64(convId);
    w.u64(userId);
    sendFrame(MsgType::GroupInvite, toBytes(w.take()));
}

// 退出这个群。群主发这帧会被服务器拒绝。
void Session::leaveGroup(quint64 convId) {
    W w;
    w.u64(convId);
    sendFrame(MsgType::GroupLeave, toBytes(w.take()));
}

// 解散这个群。只有群主能成功。
void Session::dissolveGroup(quint64 convId) {
    W w;
    w.u64(convId);
    sendFrame(MsgType::GroupDissolve, toBytes(w.take()));
}

// 发一帧空心跳，让服务器的空闲计时刷新。未连接时什么都不做。
void Session::sendHeartbeat() {
    if (sock_.state() == QAbstractSocket::ConnectedState) sendFrame(MsgType::Heartbeat, QByteArray());
}

// 套接字断开。停掉心跳，并通知窗口。
void Session::onDisconnected() {
    heartbeat_.stop();
    emit disconnected();
}

// 把套接字里的字节切成帧。半包留在 acc_，下一轮再拼
void Session::onReadyRead() {
    acc_.append(sock_.readAll());
    std::string buf = fromBytes(acc_);
    std::size_t off = 0;
    try {
        while (true) {
            auto frame = parse_frame(buf, off);
            if (!frame) break;
            handle(frame->type, toBytes(frame->payload));
        }
    } catch (const std::exception&) {
        sock_.disconnectFromHost();
        return;
    }
    if (off > 0) acc_ = acc_.mid(static_cast<int>(off));
}

// 按帧类型把载荷变成信号。会话列表版本不比已有的新，就整份丢掉
void Session::handle(MsgType type, const QByteArray& payload) {
    R r(fromBytes(payload));
    if (type == MsgType::RegisterOk) {
        quint64 id = 0;
        if (take64(r, id)) emit registerOk(id);
        return;
    }
    if (type == MsgType::LoginOk) {
        quint64 id = 0;
        if (take64(r, id)) emit loginOk(id);
        return;
    }
    if (type == MsgType::LoginFail || type == MsgType::Error) {
        std::string s;
        r.str(s);
        uint64_t nonce = 0, msg = 0;
        if (r.u64(nonce) && r.u64(msg)) {
            log_error("chat 失败 nonce=" + std::to_string(nonce) + " msg=" + std::to_string(msg) + " " + s);
        } else if (type == MsgType::LoginFail) {
            log_error("auth 失败 " + s);
        } else {
            log_error("请求失败 " + s);
        }
        emit failed(QString::fromStdString(s));
        return;
    }
    if (type == MsgType::QueueBusy) {
        std::string s;
        r.str(s);
        uint64_t nonce = 0, msg = 0;
        if (r.u64(nonce) && r.u64(msg)) {
            log_error("chat 失败 nonce=" + std::to_string(nonce) + " msg=" + std::to_string(msg) + " " + s);
        } else {
            log_error("queue 失败 " + s);
        }
        emit queueBusy(QString::fromStdString(s));
        return;
    }
    if (type == MsgType::Kick) {
        std::string s;
        r.str(s);
        log_error("auth 顶号 " + s);
        emit kicked(QString::fromStdString(s));
        sock_.disconnectFromHost(); // 窗口先收到 kicked，据此不要自动重连
        return;
    }
    if (type == MsgType::ConvList) {
        quint64 rev = 0, n = 0;
        if (!take64(r, rev) || !take64(r, n)) return;
        if (rev <= listRev_) return; // 后到的旧列表不能盖住已经接受的新列表
        QVector<ConvInfo> next;
        for (quint64 i = 0; i < n; ++i) {
            ConvInfo c;
            uint8_t kindb = 0;
            quint64 nm = 0;
            std::string title;
            if (!take64(r, c.id) || !r.u8(kindb) || !r.str(title)) return;
            c.kind = kindb;
            c.title = QString::fromStdString(title);
            if (!take64(r, c.lastRead) || !take64(r, c.latest) || !take64(r, c.owner) || !take64(r, nm)) return;
            for (quint64 k = 0; k < nm; ++k) {
                MemberInfo m;
                std::string name, pub;
                if (!take64(r, m.id) || !r.str(name) || !r.str(pub)) return;
                m.name = QString::fromStdString(name);
                m.pub = toBytes(pub);
                c.members.push_back(m);
            }
            next.push_back(c);
        }
        listRev_ = rev;
        convs_ = next;
        emit conversationsChanged();
        return;
    }
    if (type == MsgType::Push || type == MsgType::SyncBatch) {
        auto one = [&](R& in) {
            Incoming m;
            std::string cipher;
            if (!take64(in, m.msgId) || !take64(in, m.convId) || !take64(in, m.senderId) || !take64(in, m.ts) || !in.str(cipher))
                return false;
            m.cipher = toBytes(cipher);
            emit incomingMessage(m);
            return true;
        };
        if (type == MsgType::Push) {
            one(r);
            return;
        }
        quint64 n = 0;
        if (!take64(r, n)) return;
        quint64 last = 0;
        for (quint64 i = 0; i < n; ++i) {
            Incoming m;
            std::string cipher;
            if (!take64(r, m.msgId) || !take64(r, m.convId) || !take64(r, m.senderId) || !take64(r, m.ts) || !r.str(cipher)) return;
            m.cipher = toBytes(cipher);
            last = m.msgId;
            emit incomingMessage(m);
        }
        if (n == 100 && last > 0) requestSync(last); // 服务器一次最多 100 条，满了就接着要
        return;
    }
    if (type == MsgType::ChatAck) {
        quint64 nonce = 0, id = 0;
        if (take64(r, nonce) && take64(r, id)) emit messageAck(nonce, id);
        return;
    }
    if (type == MsgType::Receipt) {
        quint8 kind = 0;
        quint64 conv = 0, id = 0, from = 0;
        if (r.u8(kind) && take64(r, conv) && take64(r, id) && take64(r, from)) emit receiptArrived(kind, conv, id, from);
        return;
    }
    if (type == MsgType::FriendSearchResult) {
        quint8 rel = 0;
        quint64 id = 0;
        std::string name;
        if (!r.u8(rel) || !take64(r, id) || !r.str(name)) return;
        emit searchResult(rel, id, QString::fromStdString(name));
        return;
    }
    if (type == MsgType::FriendRequest) return; // 空载荷表示申请已被收下，按钮文案由窗口自己改
}
