# 编译器和标准
CXX = g++-13
CXXFLAGS = -std=c++23 -Wno-deprecated-declarations -O2
LDFLAGS = -lssl -lcrypto

# 源文件
CLIENT_SRCS = client/cli.cpp
SERVER_SRCS = server/srv.cpp
COMMON_SRCS = common/crypto.cpp common/Send.cpp
TEST_SRCS = stress_test/stest.cpp

# 对应的目标文件
CLIENT_OBJS = $(CLIENT_SRCS:.cpp=.o)
SERVER_OBJS = $(SERVER_SRCS:.cpp=.o)
COMMON_OBJS = $(COMMON_SRCS:.cpp=.o)
TEST_OBJS = $(TEST_SRCS:.cpp=.o)

# 最终可执行文件
TARGET_SRV = srv
TARGET_CLI = cli
TARGET_TEST = stest


# 声明伪目标
.PHONY: all clean


all: $(TARGET_SRV) $(TARGET_CLI) $(TARGET_TEST)

# 构建服务端
$(TARGET_SRV): $(SERVER_OBJS) $(COMMON_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# 构建客户端
$(TARGET_CLI): $(CLIENT_OBJS) $(COMMON_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# 构建测试程序
$(TARGET_TEST): $(TEST_OBJS) $(COMMON_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# 编译规则
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -Icommon -c $< -o $@

# 清理
clean:
	rm -f $(TARGET_SRV) $(TARGET_CLI) $(TARGET_TEST) *.o */*.o