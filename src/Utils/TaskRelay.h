#pragma once

#include "SingleThreadExecutor.h"

namespace PrismaUI::Utils {
    // Producers only touch the executor's short local queue lock. The native
    // scheduler may block, so submission to it happens exclusively on the worker.
    class TaskRelay {
    public:
        enum class Destination { Game, UI };
        using Task = std::function<void()>;
        using Submit = std::function<void(Destination, Task)>;
        using OnError = std::function<void(std::exception_ptr)>;

        TaskRelay(Submit submit, OnError onError) :
            submit_(std::move(submit)), onError_(std::move(onError)) {}

        void Post(Destination destination, Task task) {
            // No future.get(): rendering may own a lock needed by an existing
            // native task. FIFO preserves each producer's mask/unmask ordering.
            worker_.submit([this, destination, task = std::move(task)]() mutable {
                try {
                    submit_(destination, std::move(task));
                } catch (...) {
                    onError_(std::current_exception());
                }
            });
        }

    private:
        Submit submit_;
        OnError onError_;
        // Destroyed first, so queued submissions drain before their sinks die.
        SingleThreadExecutor worker_;
    };
}
