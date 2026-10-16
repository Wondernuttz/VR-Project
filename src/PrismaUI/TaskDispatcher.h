#pragma once

#include <functional>

namespace PrismaUI::Tasks {
    bool Initialize();

    // Asynchronous submission; callbacks still execute via SKSE, never here.
    // Do not wait for delivery while holding renderer/browser locks.
    void AddTask(std::function<void()> task);
    void AddUITask(std::function<void()> task);
}
