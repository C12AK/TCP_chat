#include "thread_pool.h"


// 拉起 thread_num 个工人。每个工人睡在条件变量上，被叫醒且队列非空就取一个任务执行。
ThreadPool::ThreadPool(std::size_t thread_num) : stop(false) {
    for (std::size_t i = 0; i < thread_num; ++i) {
        workers.emplace_back([this] {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(queue_mtx);
                    // 谓词为假就继续睡。内核有时会在没有 notify 时叫醒线程
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


// 拒绝新任务，叫醒所有工人，等他们把队列里已有的任务跑完。
ThreadPool::~ThreadPool() {
    {
        std::lock_guard lock(queue_mtx);
        stop = true;
    }
    cv.notify_all();
    for (std::thread& worker : workers) worker.join();
}
