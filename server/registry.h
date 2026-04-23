#ifndef REGISTRY_H
#define REGISTRY_H

#include "common.h"

#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

// 线程安全的用户名 -> 连接信息映射
//   Reactor 线程写（注册 / 注销）
//   worker 线程读（转发目标查询）
//   aeskey 为 shared_ptr<const>，安全跨线程复制
class Registry {
  public:
    struct Entry {
        int fd;
        int gen;
        std::shared_ptr<const vecuc> aeskey;
    };

    // 若用户名已存在返回 false，不覆盖
    bool try_insert(const std::string& user, Entry e);

    // 仅当 {fd, gen} 精确匹配时才删除，避免误删被顶替的新连接
    void erase_if_match(const std::string& user, int fd, int gen);

    std::optional<Entry> lookup(const std::string& user) const;

  private:
    mutable std::shared_mutex mtx;
    std::unordered_map<std::string, Entry> map;
};

#endif // REGISTRY_H
