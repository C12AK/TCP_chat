# ============ C++（服务端） ============
CXX = g++-13
CXXFLAGS = -std=c++23 -Wno-deprecated-declarations -O2 -MMD -MP
LDFLAGS = -lssl -lcrypto -lpthread /usr/lib/x86_64-linux-gnu/libsqlite3.so.0
INCLUDES = -Icommon -Iserver

COMMON_SRCS = common/aes.cpp common/protocol.cpp common/net.cpp common/log.cpp common/pass.cpp common/e2e.cpp
SERVER_SRCS = server/srv.cpp server/reactor.cpp server/thread_pool.cpp server/db.cpp

COMMON_OBJS = $(COMMON_SRCS:.cpp=.o)
SERVER_OBJS = $(SERVER_SRCS:.cpp=.o)

DEPS = $(COMMON_OBJS:.o=.d) $(SERVER_OBJS:.o=.d)

TARGET_SRV = srv

# ============ 测试 ============
# 用国内镜像作为默认值，海外用户可在命令行覆盖：make bench GOPROXY=https://proxy.golang.org,direct
GOPROXY ?= https://goproxy.cn,direct

UNIT_BIN = tests/unit_tests
INTEGRATION_BIN = tests/integration_tests
BENCH_BIN = tests/bench/server_bench

INTEGRATION_OBJS = common/protocol.o common/net.o common/e2e.o common/aes.o

# ============ 伪目标 ============
.PHONY: all test test-unit test-integration bench clean

all: $(TARGET_SRV)

test: test-unit test-integration

test-unit: $(UNIT_BIN)

test-integration: $(INTEGRATION_BIN)

$(UNIT_BIN): tests/unit.cpp $(filter-out server/srv.o server/reactor.o,$(SERVER_OBJS)) $(COMMON_OBJS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@ $(LDFLAGS)

$(INTEGRATION_BIN): tests/integration.cpp $(INTEGRATION_OBJS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@ -lssl -lcrypto -lpthread

# ---- 构建服务端 ----
$(TARGET_SRV): $(SERVER_OBJS) $(COMMON_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# ---- 编译单个 .cpp ----
%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

-include $(DEPS)

# ---- 构建压测程序 ----
# 首次会联网拉依赖，之后就本地编译。
bench: $(BENCH_BIN)

$(BENCH_BIN): tests/bench/main.go tests/bench/go.mod
	cd tests/bench && GOPROXY=$(GOPROXY) go mod tidy && GOPROXY=$(GOPROXY) go build -o server_bench .

# ---- 清理 ----
clean:
	rm -f $(TARGET_SRV) $(UNIT_BIN) $(INTEGRATION_BIN) $(BENCH_BIN)
	rm -f *.o */*.o *.d */*.d
