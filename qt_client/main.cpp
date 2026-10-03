#include "log.h"
#include "login_window.h"

#include <QApplication>
#include <QDir>

// 打开客户端滚动日志后进入登录窗。聊天窗口由登录成功后再创建
int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QString root = QDir::homePath() + "/.local/share/tcp_chat";
    QDir().mkpath(root);
    log_open((root + "/client.log").toStdString(), 8u * 1024u * 1024u);
    log_info("客户端启动");
    LoginWindow w;
    w.show();
    return app.exec();
}
