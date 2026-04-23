#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>


// 固定工作线程数的线程池。任务为任意可调用对象
class ThreadPool {
  public:
    explicit ThreadPool(std::size_t thread_num);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // 关停后静默丢弃新任务，避免退出时阻塞调用方
    template<class F>
    void enqueue(F&& f) {
        {
            std::lock_guard lock(queue_mtx);
            if (stop) return;
            task_queue.emplace(std::forward<F>(f));
        }
        cv.notify_one();
    }

  private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> task_queue;
    std::mutex queue_mtx;
    std::condition_variable cv;
    std::atomic<bool> stop;
};

#endif // THREAD_POOL_H
