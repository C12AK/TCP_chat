#include "thread_pool.h"


ThreadPool::ThreadPool(std::size_t thread_num) : stop(false) {
    for (std::size_t i = 0; i < thread_num; ++i) {
        workers.emplace_back([this] {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(queue_mtx);
                    cv.wait(lock, [this] { return stop || !task_queue.empty(); });
                    if (stop && task_queue.empty()) return;      // 执行完所有任务才退出
                    task = std::move(task_queue.front());
                    task_queue.pop();
                }
                task();
            }
        });
    }
}


ThreadPool::~ThreadPool() {
    {
        std::lock_guard lock(queue_mtx);
        stop = true;
    }
    cv.notify_all();
    for (std::thread& worker : workers) worker.join();
}
