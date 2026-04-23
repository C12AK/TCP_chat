# ============ C++（服务端 / 客户端） ============
CXX = g++-13
CXXFLAGS = -std=c++23 -Wno-deprecated-declarations -O2 -MMD -MP
LDFLAGS = -lssl -lcrypto -lpthread
INCLUDES = -Icommon -Iserver

COMMON_SRCS = common/crypto.cpp common/aes.cpp common/protocol.cpp common/net.cpp
SERVER_SRCS = server/srv.cpp server/reactor.cpp server/registry.cpp server/thread_pool.cpp
CLIENT_SRCS = client/cli.cpp

COMMON_OBJS = $(COMMON_SRCS:.cpp=.o)
SERVER_OBJS = $(SERVER_SRCS:.cpp=.o)
CLIENT_OBJS = $(CLIENT_SRCS:.cpp=.o)

DEPS = $(COMMON_OBJS:.o=.d) $(SERVER_OBJS:.o=.d) $(CLIENT_OBJS:.o=.d)

TARGET_SRV = srv
TARGET_CLI = cli

# ============ Go（压测脚本） ============
# 用国内镜像作为默认值，海外用户可在命令行覆盖：make stest GOPROXY=https://proxy.golang.org,direct
GOPROXY ?= https://goproxy.cn,direct

STEST_SRC = stress_test/stest.go
STEST_BIN = stress_test/stest

# ============ 伪目标 ============
.PHONY: all stest clean

all: $(TARGET_SRV) $(TARGET_CLI)

# ---- 构建服务端 ----
$(TARGET_SRV): $(SERVER_OBJS) $(COMMON_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# ---- 构建客户端 ----
$(TARGET_CLI): $(CLIENT_OBJS) $(COMMON_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# ---- 编译单个 .cpp ----
%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

-include $(DEPS)

# ---- 构建压测脚本 ----
# 首次会联网拉依赖，之后就本地编译。不懂 Go 的用户直接 `make stest` 即可。
stest: $(STEST_BIN)

$(STEST_BIN): $(STEST_SRC) stress_test/go.mod
	cd stress_test && GOPROXY=$(GOPROXY) go mod tidy && GOPROXY=$(GOPROXY) go build -o stest

# ---- 清理 ----
clean:
	rm -f $(TARGET_SRV) $(TARGET_CLI) $(STEST_BIN)
	rm -f *.o */*.o *.d */*.d
