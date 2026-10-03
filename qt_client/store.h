#ifndef STORE_H
#define STORE_H

#include "sqlite3_min.h"

#include <QString>
#include <QVector>
#include <cstdint>

struct LocalMsg {
    quint64 msgId = 0;
    quint64 convId = 0;
    quint64 senderId = 0;
    quint64 ts = 0;
    QString body;
    int status = 0;      // 1 已入库  2 已送达  3 已读
    bool pending = false; // 还没有服务器消息 id 的发送中草稿
    quint64 nonce = 0;   // 仅 pending 使用，用来等 ChatAck 对上
};

// 本机已解开的正文。路径是 ~/.local/share/tcp_chat/<用户id>/chat.db
// pending 是已发出、还没拿到服务器消息 id 的草稿；msgs 是已确认或已收到的正文
class Store {
  public:
    Store() = default;
    ~Store();
    bool open(const QString& dir);
    void addPending(quint64 convId, quint64 nonce, const QString& body);          // 发送中
    void bindAck(quint64 nonce, quint64 msgId);                                   // ChatAck 到达，草稿改成正式消息
    bool hasMessage(quint64 msgId) const;                                         // Push 和补拉可能各来一次，用来去重
    void upsert(quint64 msgId, quint64 convId, quint64 senderId, quint64 ts, const QString& body);
    void setStatus(quint64 msgId, int status);                                    // 1 已入库  2 已送达  3 已读
    void hide(quint64 msgId);                                                     // 只从本机列表藏掉，服务器上的密文还在
    QVector<LocalMsg> load(quint64 convId, const QString& needle) const;          // needle 非空时做整段包含
    quint64 maxId() const;                                                        // 上线补拉的起点

  private:
    sqlite3* db = nullptr;
};

#endif
