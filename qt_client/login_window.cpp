#include "login_window.h"

#include "e2e.h"
#include "main_window.h"

#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

// 摆好登录表单，并把按钮、注册成功、登录成功和连接成功接到对应处理。
LoginWindow::LoginWindow(QWidget* parent) : QWidget(parent) {
    setWindowTitle("登录");
    host_ = new QLineEdit("127.0.0.1");
    port_ = new QLineEdit("8080");
    user_ = new QLineEdit;
    pass_ = new QLineEdit;
    pass_->setEchoMode(QLineEdit::Password);
    hint_ = new QLabel;
    auto* reg = new QPushButton("注册");
    auto* login = new QPushButton("登录");
    auto* form = new QFormLayout;
    form->addRow("服务器", host_);
    form->addRow("端口", port_);
    form->addRow("用户名", user_);
    form->addRow("密码", pass_);
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(reg);
    buttons->addWidget(login);
    auto* lay = new QVBoxLayout(this);
    lay->addLayout(form);
    lay->addLayout(buttons);
    lay->addWidget(hint_);
    connect(reg, &QPushButton::clicked, this, &LoginWindow::onRegister);
    connect(login, &QPushButton::clicked, this, &LoginWindow::onLogin);
    connect(&session_, &Session::registerOk, this, &LoginWindow::onRegisterOk);
    connect(&session_, &Session::loginOk, this, &LoginWindow::onLoginOk);
    connect(&session_, &Session::failed, this, &LoginWindow::onFailed);
    connect(&session_, &Session::connected, this, [this] {
        if (!isVisible()) return; // 登录窗已经关掉时，迟到的连接成功不要再发帧
        if (registerMode_) session_.registerAccount(user_->text(), pass_->text(), pendingPub_);
        else session_.login(user_->text(), pass_->text());
    });
}

// 本机数据根目录。每个用户再按 id 建子目录。
QString LoginWindow::dataRoot() const {
    return QDir::homePath() + "/.local/share/tcp_chat";
}

// 记下地址和端口并发起连接。帧要等 connected 才写。
void LoginWindow::ensureConnected() {
    hostName_ = host_->text().trimmed();
    portNum_ = static_cast<quint16>(port_->text().toUInt());
    session_.open(hostName_, portNum_);
}

// 生成本机长期密钥，先留在内存里，注册成功拿到 id 再落盘。
void LoginWindow::onRegister() {
    registerMode_ = true;
    std::string priv, pub;
    if (!e2e_generate(priv, pub)) {
        hint_->setText("生成密钥失败");
        return;
    }
    pendingPriv_ = QByteArray(priv.data(), static_cast<int>(priv.size()));
    pendingPub_ = QByteArray(pub.data(), static_cast<int>(pub.size()));
    ensureConnected();
}

// 不生成密钥。连上之后只发送登录。
void LoginWindow::onLogin() {
    registerMode_ = false;
    ensureConnected();
}

// 按用户 id 创建目录，把私钥写入 identity.key。这条连接还没进入聊天。
void LoginWindow::onRegisterOk(quint64 id) {
    QString dir = dataRoot() + "/" + QString::number(id);
    QDir().mkpath(dir);
    QFile f(dir + "/identity.key");
    if (f.open(QIODevice::WriteOnly)) f.write(pendingPriv_);
    hint_->setText("注册成功，请登录。用户 id = " + QString::number(id));
}

// 读出私钥。没有就停在登录窗。有则把同一个 Session 交给主窗口，连接不断开。
void LoginWindow::onLoginOk(quint64 id) {
    QString dir = dataRoot() + "/" + QString::number(id);
    QFile f(dir + "/identity.key");
    if (!f.open(QIODevice::ReadOnly)) {
        hint_->setText("这台电脑上没有该账号的密钥，解不开历史消息");
        return;
    }
    QByteArray priv = f.readAll();
    auto* main = new MainWindow(nullptr, &session_, id, priv, dir, hostName_, portNum_, user_->text(), pass_->text());
    main->setAttribute(Qt::WA_DeleteOnClose);
    connect(main, &QObject::destroyed, this, [this] { show(); });
    main->show();
    hide();
}

// 把服务器返回的失败原因写到提示上。
void LoginWindow::onFailed(const QString& reason) {
    if (!registerMode_ && reason.isEmpty()) return; // 登录时的空原因不擦掉已经写上的提示
    hint_->setText(reason);
}
