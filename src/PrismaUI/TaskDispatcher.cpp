#include "TaskDispatcher.h"

#include "Utils/TaskRelay.h"

namespace PrismaUI::Tasks {
    namespace {
        using Relay = Utils::TaskRelay;

        Relay& GetRelay() {
            // SKSE plugins remain loaded for the process lifetime. In particular,
            // do not join this worker during render teardown or DLL detach: it
            // may be waiting for SKSE while the caller owns the renderer lock.
            static auto* relay = [] {
                auto* tasks = SKSE::GetTaskInterface();
                if (!tasks) {
                    throw std::runtime_error("SKSE task interface is unavailable");
                }
                return new Relay(
                    [tasks](Relay::Destination destination, Relay::Task task) {
                        if (destination == Relay::Destination::UI) {
                            tasks->AddUITask(std::move(task));
                        } else {
                            tasks->AddTask(std::move(task));
                        }
                    },
                    [](std::exception_ptr error) {
                        try {
                            std::rethrow_exception(error);
                        } catch (const std::exception& e) {
                            logger::critical("SKSE task submission failed: {}", e.what());
                        } catch (...) {
                            logger::critical("SKSE task submission failed: unknown exception");
                        }
                    });
            }();
            return *relay;
        }
    }

    bool Initialize() {
        try {
            (void)GetRelay();
            logger::info("PrismaUI VR Alpha 8.2: asynchronous SKSE task submission enabled");
            return true;
        } catch (const std::exception& e) {
            logger::critical("Failed to initialize SKSE task dispatcher: {}", e.what());
            return false;
        }
    }

    void AddTask(std::function<void()> task) {
        GetRelay().Post(Relay::Destination::Game, std::move(task));
    }

    void AddUITask(std::function<void()> task) {
        GetRelay().Post(Relay::Destination::UI, std::move(task));
    }
}
