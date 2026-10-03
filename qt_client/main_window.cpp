#include "main_window.h"

#include "e2e.h"
#include "log.h"

#include <string>

#include <QDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

// 搭好聊天窗，接上 Session 的信号，并按本机已有的最大消息 id 补拉。
MainWindow::MainWindow(QWidget* parent, Session* session, quint64 selfId, const QByteArray& priv, const QString& dir,
                       const QString& host, quint16 port, const QString& user, const QString& pass)
    : QMainWindow(parent), session_(session), selfId_(selfId), priv_(priv), host_(host), port_(port), user_(user), pass_(pass) {
    setWindowTitle("聊天 - " + user);
    resize(860, 560);
    store_.open(dir);

    convList_ = new QListWidget;
    msgList_ = new QListWidget;
    search_ = new QLineEdit;
    search_->setPlaceholderText("搜索当前会话（整段包含）");
    input_ = new QLineEdit;
    input_->setPlaceholderText("输入消息");
    auto* send = new QPushButton("发送");
    auto* add = new QPushButton("加好友");
    auto* del = new QPushButton("删除好友");
    auto* group = new QPushButton("建群");
    auto* invite = new QPushButton("邀请");
    leaveBtn_ = new QPushButton("退出群");
    auto* hide = new QPushButton("删除这条");
    status_ = new QLabel;

    auto* left = new QVBoxLayout;
    left->addWidget(add);
    left->addWidget(convList_);
    auto* right = new QVBoxLayout;
    auto* tools = new QHBoxLayout;
    tools->addWidget(del);
    tools->addWidget(group);
    tools->addWidget(invite);
    tools->addWidget(leaveBtn_);
    tools->addWidget(hide);
    right->addLayout(tools);
    right->addWidget(search_);
    right->addWidget(msgList_, 1);
    auto* composer = new QHBoxLayout;
    composer->addWidget(input_, 1);
    composer->addWidget(send);
    right->addLayout(composer);
    right->addWidget(status_);

    auto* lw = new QWidget;
    lw->setLayout(left);
    auto* rw = new QWidget;
    rw->setLayout(right);
    auto* split = new QSplitter;
    split->addWidget(lw);
    split->addWidget(rw);
    setCentralWidget(split);

    connect(convList_, &QListWidget::currentRowChanged, this, [this](int) { refreshMessages(); });
    connect(search_, &QLineEdit::textChanged, this, [this](const QString&) { refreshMessages(); });
    connect(send, &QPushButton::clicked, this, &MainWindow::onSend);
    connect(input_, &QLineEdit::returnPressed, this, &MainWindow::onSend);
    connect(add, &QPushButton::clicked, this, &MainWindow::openFriendDialog);
    connect(del, &QPushButton::clicked, this, &MainWindow::removeFriend);
    connect(group, &QPushButton::clicked, this, &MainWindow::makeGroup);
    connect(invite, &QPushButton::clicked, this, &MainWindow::inviteSomeone);
    connect(leaveBtn_, &QPushButton::clicked, this, &MainWindow::leaveOrDissolve);
    connect(hide, &QPushButton::clicked, this, &MainWindow::hideSelected);

    connect(session_, &Session::conversationsChanged, this, &MainWindow::refreshConvs);
    connect(session_, &Session::incomingMessage, this, &MainWindow::onIncoming);
    connect(session_, &Session::messageAck, this, [this](quint64 nonce, quint64 id) {
        store_.bindAck(nonce, id);
        refreshMessages();
    });
    connect(session_, &Session::receiptArrived, this, [this](quint8 kind, quint64, quint64 msgId, quint64) {
        if (kind == 2) store_.setStatus(msgId, 3);
        else store_.setStatus(msgId, 2);
        refreshMessages();
    });
    connect(session_, &Session::failed, this, [this](const QString& s) { status_->setText(s); });
    connect(session_, &Session::queueBusy, this, [this](const QString& s) { status_->setText(s); });
    connect(session_, &Session::kicked, this, [this](const QString& s) {
        kicked_ = true;
        QMessageBox::information(this, "下线", s);
        close();
    });
    connect(session_, &Session::disconnected, this, &MainWindow::onDisconnected);
    connect(session_, &Session::connected, this, [this] {
        if (!retrying_) return; // 第一次登录发生在登录窗，那时主窗口还不存在
        retrying_ = false;
        session_->login(user_, pass_);
    });
    connect(session_, &Session::loginOk, this, [this](quint64) { session_->requestSync(store_.maxId()); }); // 重连成功后补拉。首次登录这信号已经发过

    refreshConvs();
    session_->requestSync(store_.maxId());
}

// 左侧当前行对应的会话。没有选中或行号越界时返回空。
const ConvInfo* MainWindow::currentConv() const {
    int row = convList_->currentRow();
    if (row < 0 || row >= session_->conversations().size()) return nullptr;
    return &session_->conversations()[row];
}

// 按最新会话列表重画左侧，尽量保持原来选中的会话。
void MainWindow::refreshConvs() {
    quint64 keep = 0;
    if (const auto* c = currentConv()) keep = c->id;
    convList_->clear();
    int select = 0;
    const auto& convs = session_->conversations();
    for (int i = 0; i < convs.size(); ++i) {
        const auto& c = convs[i];
        QString mark = (c.latest > c.lastRead) ? "● " : ""; // 服务器上的最新 id 比已读游标新
        convList_->addItem(mark + c.title);
        if (c.id == keep) select = i;
    }
    if (!convs.isEmpty()) convList_->setCurrentRow(select);
    if (const auto* c = currentConv()) {
        leaveBtn_->setText(c->kind == 1 && c->owner == selfId_ ? "解散群" : "退出群");
    }
}

// 打开会话时，若还有未读，就把已读回执推到当前列表里的最新消息 id。
void MainWindow::markRead(const ConvInfo& c) {
    if (c.latest > c.lastRead) session_->sendReceipt(2, c.id, c.latest);
}

// 从本机库读出当前会话的正文，并顺手发出已读回执。
void MainWindow::refreshMessages() {
    msgList_->clear();
    const auto* c = currentConv();
    if (!c) return;
    markRead(*c);
    if (c->kind == 1 && c->owner == selfId_) leaveBtn_->setText("解散群");
    else leaveBtn_->setText("退出群");
    auto rows = store_.load(c->id, search_->text());
    for (const auto& m : rows) {
        QString who = (m.senderId == selfId_ || m.senderId == 0) ? "我" : QString::number(m.senderId);
        for (const auto& mem : c->members) {
            if (mem.id == m.senderId) who = mem.name;
        }
        QString tail;
        if (m.pending) tail = "（发送中）";
        else if (m.senderId == selfId_ && m.status >= 3) tail = "（已读）";   // 状态只升不降，已读覆盖送达
        else if (m.senderId == selfId_ && m.status >= 2) tail = "（已送达）";
        msgList_->addItem(who + ": " + m.body + tail);
        msgList_->item(msgList_->count() - 1)->setData(Qt::UserRole, QVariant::fromValue(m.msgId)); // 本机删除时从这里取消息 id
    }
}

// Push 或补拉来的密文。本机已有则丢掉；拆开后入库，别人发的再回一条送达。
void MainWindow::onIncoming(const Incoming& msg) {
    if (store_.hasMessage(msg.msgId)) return;
    std::string plain;
    if (!e2e_open(std::string(priv_.constData(), static_cast<std::size_t>(priv_.size())),
                  std::string(msg.cipher.constData(), static_cast<std::size_t>(msg.cipher.size())), plain)) {
        log_error("chat 失败 nonce=0 msg=" + std::to_string(static_cast<unsigned long long>(msg.msgId)) + " 解密失败");
        return;
    }
    quint64 sender = msg.senderId;
    store_.upsert(msg.msgId, msg.convId, sender, msg.ts, QString::fromStdString(plain));
    if (sender != selfId_) session_->sendReceipt(1, msg.convId, msg.msgId);
    refreshMessages();
    refreshConvs();
}

// 发送：当前会话的每个成员各封一份，先在本机记一条「发送中」，再把 ChatSend 交给套接字
void MainWindow::onSend() {
    const auto* c = currentConv();
    if (!c || c->kind == 2) {
        status_->setText("这个会话不能发消息");
        return;
    }
    QString text = input_->text();
    if (text.isEmpty()) return;
    quint64 nonce = QRandomGenerator::global()->generate64(); // 用来让服务器认出重复提交，不承担保密
    QVector<QPair<quint64, QByteArray>> copies;
    for (const auto& m : c->members) {
        std::string blob;
        if (!e2e_seal(std::string(m.pub.constData(), static_cast<std::size_t>(m.pub.size())), text.toStdString(), blob)) {
            log_error("chat 失败 nonce=" + std::to_string(static_cast<unsigned long long>(nonce)) + " msg=0 加密失败");
            status_->setText("加密失败");
            return;
        }
        copies.push_back({m.id, QByteArray(blob.data(), static_cast<int>(blob.size()))});
    }
    store_.addPending(c->id, nonce, text);
    session_->sendChat(c->id, nonce, copies);
    input_->clear();
    refreshMessages();
}

// 按完整用户名查关系。已申请则按钮变灰；对方申请了我则按钮变成同意。
void MainWindow::openFriendDialog() {
    QDialog dlg(this);
    dlg.setWindowTitle("添加好友");
    auto* edit = new QLineEdit;
    auto* search = new QPushButton("搜索");
    auto* info = new QLabel("输入对方的完整用户名");
    auto* action = new QPushButton("添加好友");
    action->setEnabled(false);
    auto* lay = new QVBoxLayout(&dlg);
    lay->addWidget(edit);
    lay->addWidget(search);
    lay->addWidget(info);
    lay->addWidget(action);
    quint64 target = 0;
    int relation = 255;
    connect(search, &QPushButton::clicked, this, [this, edit] { session_->searchUser(edit->text().trimmed()); });
    connect(session_, &Session::searchResult, &dlg, [&](int rel, quint64 id, const QString& name) {
        target = id;
        relation = rel;
        action->setEnabled(false);
        action->setText("添加好友");
        if (rel == 255) info->setText("没有这个用户");
        else if (rel == 4) info->setText("不能添加自己");
        else if (rel == 3) info->setText(name + " 已是好友");
        else if (rel == 1) {
            info->setText("已发送请求");
            action->setText("已发送请求");
        } else if (rel == 2) {
            info->setText(name + " 请求添加你");
            action->setText("同意");
            action->setEnabled(true);
        } else {
            info->setText(name);
            action->setEnabled(true);
        }
    });
    connect(action, &QPushButton::clicked, &dlg, [&] {
        if (relation == 2) session_->respondFriend(target, true);
        else session_->requestFriend(target);
        action->setEnabled(false);
        action->setText("已发送请求");
        info->setText("已发送请求");
    });
    dlg.exec();
}

// 当前必须是私聊。成员里除了自己和系统之外的那个 id 就是对方。
void MainWindow::removeFriend() {
    const auto* c = currentConv();
    if (!c || c->kind != 0) {
        status_->setText("请先选中一个私聊");
        return;
    }
    for (const auto& m : c->members) {
        if (m.id != selfId_ && m.id != 1) session_->deleteFriend(m.id);
    }
}

// 问一句群名，非空才发出建群请求。
void MainWindow::makeGroup() {
    bool ok = false;
    QString title = QInputDialog::getText(this, "建群", "群名", QLineEdit::Normal, "", &ok);
    if (ok && !title.trimmed().isEmpty()) session_->createGroup(title.trimmed());
}

// 先按完整用户名搜索，查到是好友才发邀请。
void MainWindow::inviteSomeone() {
    const auto* c = currentConv();
    if (!c || c->kind != 1) {
        status_->setText("请先选中一个群");
        return;
    }
    bool ok = false;
    QString name = QInputDialog::getText(this, "邀请", "好友的完整用户名", QLineEdit::Normal, "", &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    // 搜索结果回来之后才知道对方 id。关系 3 才是好友
    connect(session_, &Session::searchResult, this, [this, cId = c->id](int rel, quint64 id, const QString&) {
        if (rel == 3) session_->inviteGroup(cId, id);
        else status_->setText("只能邀请好友");
    });
    session_->searchUser(name.trimmed());
}

// 群主发解散，其他成员发退出。
void MainWindow::leaveOrDissolve() {
    const auto* c = currentConv();
    if (!c || c->kind != 1) {
        status_->setText("请先选中一个群");
        return;
    }
    if (c->owner == selfId_) session_->dissolveGroup(c->id);
    else session_->leaveGroup(c->id);
}

// 把选中消息记进本机隐藏表。还没有服务器 id 的「发送中」删不掉。
void MainWindow::hideSelected() {
    auto* item = msgList_->currentItem();
    if (!item) return;
    quint64 id = item->data(Qt::UserRole).toULongLong();
    if (id == 0) return;
    store_.hide(id);
    refreshMessages();
}

// 被顶号则不重连。其它断开大约 2 秒后重新连接。
void MainWindow::onDisconnected() {
    if (kicked_) return;
    status_->setText("连接断开，正在重连");
    QTimer::singleShot(2000, this, [this] { tryReconnect(); });
}

// 重新发起连接。连上之后由 connected 信号用记住的口令再登录。
void MainWindow::tryReconnect() {
    if (kicked_) return;
    retrying_ = true;
    session_->open(host_, port_);
}
