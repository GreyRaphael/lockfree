#pragma once
#include <array>
#include <cstddef>
#include <functional>
#include <future>
#include <iostream>
#include <thread>

#include "def.hpp"
#include "queue/mpmc.hpp"

// Define Task as a type-erased callable
using Task = std::function<void()>;

namespace lockfree {

template <size_t PoolSize, size_t QueueSize>
class threadpool {
    static_assert(PoolSize > 0, "Thread pool must have at least one thread");
    // Array of worker threads
    std::array<std::jthread, PoolSize> threads_;
    // Lock-free MPMC queue to hold tasks, unicast MaxReaderNum 1024 takes no effect
    MPMC<Task, PoolSize, 1024, lockfree::trans::unicast> tasks_;
    // Atomic flag to signal stopping
    std::atomic<bool> stopping_{false};

   public:
    // Constructor: Initializes and starts N worker threads
    threadpool() noexcept {
        for (auto& thread : threads_) {
            thread = std::jthread{&threadpool::worker, this};
        }
    }

    // Destructor: Signals all threads to stop and waits for them to finish
    ~threadpool() {
        stop();
    }

    // Disable copy and move semantics
    threadpool(const threadpool&) = delete;
    threadpool& operator=(const threadpool&) = delete;
    threadpool(threadpool&&) = delete;
    threadpool& operator=(threadpool&&) = delete;

    // Optimized Submit Method using C++20 Features
    template <typename F, typename... Args>
        requires std::invocable<F, Args...>
    auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>> {
        using return_type = std::invoke_result_t<F, Args...>;

        // Create a packaged_task with a lambda that perfectly forwards the arguments more efficient than std::bind
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            [func = std::forward<F>(f),
             ... args = std::forward<Args>(args)]() mutable -> return_type {
                return std::invoke(func, args...);
            });

        // Obtain the future from the packaged_task
        std::future<return_type> res = task->get_future();

        // Create a Task that invokes the packaged_task
        Task wrapper = [task]() { (*task)(); };

        // Enqueue the task into the MPMC queue
        while (!tasks_.push(std::move(wrapper))) {
            // If the queue is full, yield and retry
            std::this_thread::yield();
        }

        return res;
    }

   private:
    // Worker function executed by each thread
    void worker(std::stop_token stoken) {
        while (!stopping_.load(std::memory_order_acquire) && !stoken.stop_requested()) {
            // Attempt to pop a task from the queue
            std::optional<Task> task_opt = tasks_.pop();
            if (task_opt) {
                try {
                    // Execute the task
                    (*task_opt)();
                } catch (const std::exception& e) {
                    // Handle exceptions (e.g., log them)
                    std::cerr << "Task exception: " << e.what() << '\n';
                } catch (...) {
                    std::cerr << "Task threw an unknown exception.\n";
                }
            } else {
                // If no task is available, yield to reduce CPU usage
                std::this_thread::yield();
            }
        }

        // Drain remaining tasks before exiting
        while (auto task_opt = tasks_.pop()) {
            try {
                (*task_opt)();
            } catch (const std::exception& e) {
                std::cerr << "Task exception during draining: " << e.what() << '\n';
            } catch (...) {
                std::cerr << "Task threw an unknown exception during draining.\n";
            }
        }
    }

    // Method to stop the thread pool
    void stop() noexcept {
        bool expected = false;
        if (stopping_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            // Request stop on all threads
            for (auto& thread : threads_) {
                if (thread.joinable()) {
                    thread.request_stop();
                }
            }
        }
    }
};
}  // namespace lockfree