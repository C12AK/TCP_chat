# 高性能加密即时聊天服务器
> 一组基于 TCP 的 C/S 架构聊天程序，实现了高并发的多用户一对一加密即时私聊。

***

## 项目特点
- 服务端采用单线程 Reactor 模式，基于 epoll 和线程池实现万级 QPS
  - 所有 socket I/O 与连接状态由 Reactor 线程独占，线程池仅承担 CPU 密集的 AES 加解密
  - 握手全流程非阻塞，且设有超时保护，异常 / 慢速客户端不会拖累服务
- 设计应用层协议，既解决了粘包问题，也支持发送任意长度的消息
- 借助 OpenSSL 库，实现了服务端与客户端之间的 ECDH 密钥协商和 AES-256-GCM 加密通信

***

## 环境要求

### 1. 操作系统
- **Linux**（推荐 Ubuntu 22.04 / Debian 12 / CentOS Stream 9）

### 2. 编译工具
- `g++` 13.1.0+ / `clang++` 15+ （需支持 C++23）
- GNU Make 4.3+
- **Go 1.18+**（仅压测脚本需要；不跑压测可不装）

### 3. 依赖库
- **OpenSSL 3.0+**

***

## 快速开始
### 1. 克隆仓库
```bash
git clone https://github.com/C12AK/TCP_chat
cd TCP_chat
```

### 2. 安装依赖
以 Ubuntu 22.04 为例
```bash
sudo apt update
sudo apt install build-essential g++-13 libssl-dev
sudo apt install golang-go    # 仅压测需要，可选
```

### 3. 编译
```bash
make
```

### 4. 运行
启动服务端：
```
./srv <端口号>
```
如：
```bash
./srv 8080
```

启动客户端：
```
./cli <服务器 IPv4 地址> <服务器端口号> <用户名 (长度不超过 500 字节)>
```
如：
```bash
./cli 127.0.0.1 8080 C12AK
```
客户端成功与服务器建立连接后，会出现提示消息。

### 5. 使用方法
客户端的每次操作如下：
- 给谁发消息？输入他的用户名（一行，不超过 500 字节）；
- 输入消息内容（一行，任意长度）。

如，任一客户端要给用户 C12AK 发 “我是奶龙” ：
```
C12AK
我是奶龙
```

### 6. 清除编译产物
```bash
make clean
```

***

## 压力测试
用于评估服务端在稳态下的吞吐。脚本位于 `stress_test/stest.go`，每个模拟客户端会完成完整的握手，然后给自己连发若干条消息（每条等回显再发下一条）。连接与握手阶段不计入 QPS，只有所有客户端都就绪后才开始计时。

### 1. 构建
```bash
make stest
```
首次会自动联网拉取 Go 模块依赖（默认走 `goproxy.cn` 国内镜像；海外用户可用 `make stest GOPROXY=https://proxy.golang.org,direct`）。

### 2. 使用方法
括号内为默认值
```
./stress_test/stest <连接数(1000)> <每个连接发消息数(100)> <每条消息长度(2000)> <服务器IP(127.0.0.1)> <服务器端口(8080)>
```

例如：
```bash
./stress_test/stest
./stress_test/stest 2000 200 500
```

运行前建议 `ulimit -n 65536` 以避免文件描述符耗尽。

### 3. 输出示例
```
clients=1000 loops=100 length=2000 addr=127.0.0.1:8080
----------------------------------------
Clients   total=1000  ok=1000  conn_err=0  run_err=0
Loops     100 per client, length=2000 B
Messages  100000 (echo 往返不另计)
Setup     272.02 ms  (connect + handshake，不计入 QPS)
Run       6933.53 ms  (稳态测试时长)
QPS       14422.67 msgs/sec
AvgLat    0.0693 ms/msg
----------------------------------------
```
`QPS` 即服务端每秒处理的消息数，`AvgLat` 为端到端平均单次往返耗时。

***

## 注意事项
终端可能对输入的单个字符串长度有限制（通常为 2048/4096 字节），过长会截断并丢弃后续部分，导致无法体现 “支持发送任意长度消息” 。
