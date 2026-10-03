#ifndef LOGIN_WINDOW_H
#define LOGIN_WINDOW_H

#include "session.h"

#include <QWidget>

class QLineEdit;
class QLabel;

// 登录窗。注册时在本机生成长期密钥，成功后按用户 id 写入 identity.key。
// 登录成功才把同一个 Session 交给主窗口，连接不断开。
class LoginWindow : public QWidget {
    Q_OBJECT
  public:
    explicit LoginWindow(QWidget* parent = nullptr);

  private slots:
    void onRegister();
    void onLogin();
    void onRegisterOk(quint64 id);
    void onLoginOk(quint64 id);
    void onFailed(const QString& reason);

  private:
    QString dataRoot() const;
    void ensureConnected();

    Session session_;
    QLineEdit* host_ = nullptr;
    QLineEdit* port_ = nullptr;
    QLineEdit* user_ = nullptr;
    QLineEdit* pass_ = nullptr;
    QLabel* hint_ = nullptr;
    QByteArray pendingPriv_; // 注册时刚生成的私钥，拿到用户 id 后才写入 identity.key
    QByteArray pendingPub_;
    bool registerMode_ = false; // 为真时，连接成功后发注册；否则发登录
    QString hostName_;
    quint16 portNum_ = 8080;
};

#endif
