#include "log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace {

std::mutex g_mu;             // 服务端工人和 Reactor 会同时打日志
std::string g_path;
std::size_t g_max_total = 0; // 打开时给定的全部文件合计上限
std::size_t g_file_cap = 0;  // 单个文件写到这里就轮转
std::ofstream g_out;
std::size_t g_written = 0;   // 当前这个文件已经写入的字节

// 本地时间戳。localtime_r 把结果写进调用方的 tm，多线程可以同时调用。
std::string stamp() {
    using clock = std::chrono::system_clock;
    auto t = clock::to_time_t(clock::now());
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

// 文件还不存在时当作 0。第一次打开不能因为没有旧文件而失败。
std::size_t file_size(const std::string& path) {
    std::error_code ec;
    auto n = std::filesystem::file_size(path, ec);
    if (ec) return 0;
    return static_cast<std::size_t>(n);
}

// 单文件写满 g_file_cap 后改名为 .1，更旧的依次后移，超过份数的删掉。合计大约不超过打开时给的上限
void rotate_unlocked() {
    g_out.close();
    const int keep = 3; // 旧文件留 .1 .2 .3，再老的删掉
    std::filesystem::remove(g_path + "." + std::to_string(keep));
    for (int i = keep - 1; i >= 1; --i) {
        std::error_code ec;
        std::filesystem::rename(g_path + "." + std::to_string(i),
                                g_path + "." + std::to_string(i + 1), ec);
    }
    std::error_code ec;
    std::filesystem::rename(g_path, g_path + ".1", ec);
    g_out.open(g_path, std::ios::app);
    g_written = 0;
}

// 调用方已持有 g_mu。超限先轮转再写，并立刻把用户态缓冲推进内核。
void write_unlocked(const char* level, const std::string& s) {
    if (!g_out.is_open()) return;
    std::string line = stamp() + " " + level + " " + s + "\n";
    if (g_written + line.size() > g_file_cap) rotate_unlocked();
    g_out << line;
    g_out.flush();
    g_written += line.size();
}

}  // namespace

// 创建父目录并追加打开。单文件上限约为总上限的四分之一，再保底 256 字节。
bool log_open(const std::string& path, std::size_t max_total) {
    std::lock_guard lock(g_mu);
    std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    g_path = path;
    g_max_total = max_total;
    g_file_cap = max_total / 4;
    if (g_file_cap < 256) g_file_cap = 256;
    g_out.open(g_path, std::ios::app);
    g_written = file_size(g_path);
    return g_out.is_open();
}

// 记一条普通日志。
void log_info(const std::string& s) {
    std::lock_guard lock(g_mu);
    write_unlocked("INFO", s);
}

// 记一条错误日志。
void log_error(const std::string& s) {
    std::lock_guard lock(g_mu);
    write_unlocked("ERROR", s);
}
