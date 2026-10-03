#include "log.h"
#include "login_window.h"

#include <QApplication>
#include <QDir>

// 打开客户端滚动日志后进入登录窗。聊天窗口由登录成功后再创建
int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QString root = QDir::homePath() + "/.local/share/tcp_chat";
    QDir().mkpath(root);
    // 只记失败。合计 1MB，不按服务器的日志体积占用用户磁盘。
    log_open((root + "/client.log").toStdString(), 1u * 1024u * 1024u);
    log_info("客户端启动");
    LoginWindow w;
    w.show();
    return app.exec();
}
