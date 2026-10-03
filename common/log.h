#ifndef LOG_H
#define LOG_H

#include <cstddef>
#include <string>

// 打开滚动日志。单个文件写满后改名轮转，所有文件合计不超过 max_total 字节。
bool log_open(const std::string& path, std::size_t max_total);

void log_info(const std::string& s);
void log_error(const std::string& s);

#endif
