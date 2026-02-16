#!/bin/bash
CONCURRENCY=${1:-3000}   # 默认 3000 并发
HOST=${2:-127.0.0.1}
PORT=${3:-8080}

echo "Starting $CONCURRENCY clients to $HOST:$PORT..."

# 启动并发客户端
for i in $(seq 1 $CONCURRENCY); do
    username="user_$i"  # 唯一用户名

    (
        # 使用 timeout 限制单个客户端运行时间（避免 hang 住）
        # 将用户名作为输入或参数传入（根据你 cli 的实际逻辑调整）
        timeout 10s ./cli "$HOST" "$PORT" "$username" >/dev/null 2>&1   # /dev/null 是黑洞设备，用于静默运行程序
        if [ $? -eq 0 ]; then
            echo -n "."         # . 代表成功
        else
            echo -n "X"         # X 代表失败
        fi
    ) &
done

wait
echo -e "\nTest finished."