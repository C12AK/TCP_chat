#ifndef SESSION_H
#define SESSION_H

#include "protocol.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTcpSocket>
#include <QTimer>
#include <QVector>

struct MemberInfo {
    quint64 id = 0;
    QString name;
    QByteArray pub;
};

struct ConvInfo {
    quint64 id = 0;
    int kind = 0; // 0 私聊  1 群  2 与系统的会话
    QString title;
    quint64 lastRead = 0;
    quint64 latest = 0;
    quint64 owner = 0;
    QVector<MemberInfo> members;
};

struct Incoming {
    quint64 msgId = 0;
    quint64 convId = 0;
    quint64 senderId = 0;
    quint64 ts = 0;
    QByteArray cipher;
};

// 客户端套接字。界面只调这些方法、听这些信号，不直接读写 TCP。
// 收到的帧在 handle() 里按 MsgType 拆开；要发出去的帧在 sendFrame() 里组好。
class Session : public QObject {
    Q_OBJECT
  public:
    explicit Session(QObject* parent = nullptr);

    void open(const QString& host, quint16 port);
    void registerAccount(const QString& user, const QString& pass, const QByteArray& pub);
    void login(const QString& user, const QString& pass);
    void requestSync(quint64 afterId);
    void sendChat(quint64 convId, quint64 nonce, const QVector<QPair<quint64, QByteArray>>& copies);
    void sendReceipt(quint8 kind, quint64 convId, quint64 msgId);
    void searchUser(const QString& name);
    void requestFriend(quint64 id);
    void respondFriend(quint64 requester, bool accept);
    void deleteFriend(quint64 id);
    void createGroup(const QString& title);
    void inviteGroup(quint64 convId, quint64 userId);
    void leaveGroup(quint64 convId);
    void dissolveGroup(quint64 convId);

    const QVector<ConvInfo>& conversations() const { return convs_; }

  signals:
    void registerOk(quint64 id);
    void loginOk(quint64 id);
    void failed(const QString& reason);
    void kicked(const QString& reason);
    void conversationsChanged();
    void incomingMessage(const Incoming& msg);
    void messageAck(quint64 nonce, quint64 msgId);
    void receiptArrived(quint8 kind, quint64 convId, quint64 msgId, quint64 from);
    void searchResult(int relation, quint64 id, const QString& name);
    void queueBusy(const QString& reason);
    void disconnected();
    void connected();

  private slots:
    void onReadyRead();
    void onDisconnected();
    void sendHeartbeat();

  private:
    void sendFrame(MsgType type, const QByteArray& payload);
    void handle(MsgType type, const QByteArray& payload);

    QTcpSocket sock_;
    QTimer heartbeat_;
    QByteArray acc_;            // 还没切成帧的入站字节
    QVector<ConvInfo> convs_;
    quint64 listRev_ = 0;       // 已经接受的会话列表版本，不比它新的整份丢掉
    bool wantHeartbeat_ = false; // 登录请求发出后置位。真正发心跳只看套接字是否已连接
};

#endif
