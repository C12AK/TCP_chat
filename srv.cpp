#include <bits/stdc++.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
using namespace std;

#define BUFSZ 1024
char buf[BUFSZ];
vector<int> clis;
map<string, int> usr2sock;
map<int, string> sock2usr;

string src, tgt, msg;

inline void split_msg(int len){
    tgt = msg = "";
    int tgtlen = 0, i = 0;
    while(buf[i] != '#' && i < len){
        tgtlen = tgtlen * 10 + (buf[i] - '0');
        i++;
    }
    i++;
    while(tgtlen-- && i < len) tgt += buf[i++];
    while(i < len) msg += buf[i++];
}

int main(int argc, char *argv[]){
    if(argc != 2){
        cerr << format("Usage: {} <Port>\n", argv[0]);
        exit(1);
    }

    int srv_sock = socket(PF_INET, SOCK_STREAM, 0);
    if(srv_sock < 0){
        cerr << "Socket creating failed.\n";
        exit(1);
    }

    sockaddr_in srv_addr{};
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    srv_addr.sin_port = htons(atoi(argv[1]));

    if(bind(srv_sock, (sockaddr *)&srv_addr, sizeof(srv_addr)) < 0){
        cerr << "Binding failed.\n";
        exit(1);
    }
    if(listen(srv_sock, 5) < 0){
        cerr << "Listening failed.\n";
        exit(1);
    }
    cout << "Server started, waiting for connection..." << endl;

    fd_set fds, tot_fds;
    FD_ZERO(&tot_fds);
    FD_SET(srv_sock, &tot_fds);
    int mxfd = srv_sock;

    while(1){
        fds = tot_fds;
        int ready = select(mxfd + 1, &fds, nullptr, nullptr, nullptr);
        if(ready < 0){
            cerr << "Select error.\n";
            break;
        }

        // 用监听 socket ：如果有新连接
        if(FD_ISSET(srv_sock, &fds)){
            sockaddr_in cli_addr{};
            socklen_t cli_addr_sz = sizeof(cli_addr);
            int cli_sock = accept(srv_sock, (sockaddr *)&cli_addr, &cli_addr_sz);
            if(cli_sock >= 0){
                FD_SET(cli_sock, &tot_fds);
                clis.push_back(cli_sock);
                mxfd = max(mxfd, cli_sock);

                // 接收客户端第三个参数（用户名）
                int len = recv(cli_sock, buf, BUFSZ - 1, 0);
                buf[len] = '\0';
                usr2sock[buf] = cli_sock, sock2usr[cli_sock] = buf;

                cout << format("Client connected, username: {}", buf) << endl;
            }
        }

        for(auto it = clis.begin(); it != clis.end(); it++){
            int u = *it;

            // 如果当前遍历到的客户端有数据过来
            if(FD_ISSET(u, &fds)){
                int len = recv(u, buf, BUFSZ - 1, 0);
                src = sock2usr[u];

                // 必须带等号
                if(len <= 0){
                    cerr << format("Client {} disconnected.\n", src);
                    close(u);
                    FD_CLR(u, &tot_fds);

                    usr2sock.erase(src), sock2usr.erase(u);

                    it = clis.erase(it);
                    it--;
                    continue;
                }

                buf[len] = '\0';
                split_msg(len);
                cout << format("\nFrom: {}\nTo: {}\nContent: {}\n", src, tgt, msg) << endl;

                // 发给对应的客户端，拼接消息：【发件人长度】#【发件人】【消息内容】
                int v = usr2sock[tgt];
                msg = to_string(src.length()) + "#" + src + msg;
                send(v, msg.c_str(), msg.length(), 0);
            }
        }
    }

    close(srv_sock);
    cout << "Exited." << endl;
    return 0;
}