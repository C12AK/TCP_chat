#include "registry.h"

#include <mutex>


bool Registry::try_insert(const std::string& user, Entry e) {
    std::unique_lock lock(mtx);
    auto [it, ok] = map.emplace(user, std::move(e));
    return ok;
}


void Registry::erase_if_match(const std::string& user, int fd, int gen) {
    std::unique_lock lock(mtx);
    auto it = map.find(user);
    if (it == map.end()) return;
    if (it->second.fd == fd && it->second.gen == gen) map.erase(it);
}


std::optional<Registry::Entry> Registry::lookup(const std::string& user) const {
    std::shared_lock lock(mtx);
    auto it = map.find(user);
    if (it == map.end()) return std::nullopt;
    return it->second;
}
