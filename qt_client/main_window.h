#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include "session.h"
#include "store.h"

#include <QByteArray>
#include <QMainWindow>

class QListWidget;
class QLineEdit;
class QLabel;
class QPushButton;

// 主界面。左边是会话（圆点表示有比已读位置更新的消息），右边是解开后的正文。
// 发出去的字先在本机按每位成员的公钥各封一份，再交给 Session。
class MainWindow : public QMainWindow {
    Q_OBJECT
  public:
    MainWindow(QWidget* parent, Session* session, quint64 selfId, const QByteArray& priv, const QString& dir,
               const QString& host, quint16 port, const QString& user, const QString& pass);

  private slots:
    void refreshConvs();     // 用最新会话列表重画左侧。当前选中的会话尽量保持
    void refreshMessages();  // 从本机库读当前会话，并顺手把已读回执发出去
    void onIncoming(const Incoming& msg); // Push 或补拉来的密文，拆开后入库
    void onSend();
    void openFriendDialog();
    void removeFriend();
    void makeGroup();
    void inviteSomeone();
    void leaveOrDissolve();   // 群主走解散，其他人走退出
    void hideSelected();
    void onDisconnected();
    void tryReconnect();      // 被顶号时不重连

  private:
    const ConvInfo* currentConv() const;
    void markRead(const ConvInfo& c);
    void resendPending(); // 把「发送中」用原来的编号再交一次。名单还没到就先留着

    Session* session_;
    Store store_;
    quint64 selfId_;
    QByteArray priv_;
    QString host_;
    quint16 port_;
    QString user_;
    QString pass_;
    bool kicked_ = false;   // 被顶号。为真时断开后不要重连，否则会把新登录再踢下去
    bool retrying_ = false; // 正在主动重连。连上之后才在主窗口里重新登录
    bool resendAfterList_ = false; // 登录后还要把「发送中」再交一次，等会话名单到了再交
    QListWidget* convList_ = nullptr;
    QListWidget* msgList_ = nullptr;
    QLineEdit* search_ = nullptr;
    QLineEdit* input_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* leaveBtn_ = nullptr;
};

#endif
