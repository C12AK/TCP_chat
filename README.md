# 基于 TCP 的聊天软件
> 一个基于 TCP 的轻量级聊天软件，实现了 C/S 架构的多用户一对一加密私聊。

***

## 项目特点
- 服务端采用单线程 Reactor 模式，基于 epoll 和线程池实现高并发
- 客户端利用 select 提供即时聊天体验
- 使用 OpenSSL 支持的 AES-256-GCM 加密通信；使用 RSA 进行 AES 密钥交换
- 支持发送无限长度的消息

***

## 环境要求

### 1. 操作系统
- **Linux**（推荐 Ubuntu 22.04 / Debian 12 / CentOS Stream 9）
- **macOS**（Intel 或 Apple Silicon 均支持，需 Xcode Command Line Tools）

### 2. 编译工具
- `g++` 13.1.0+ / `clang++` 15+ （需支持 C++23）
- GNU Make 4.3+

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

***

## 注意事项
终端可能对输入的单个字符串长度有限制（通常为 2048/4096 字节），过长会截断并丢弃后续部分，导致无法体现 “支持发送任意长度消息” 。