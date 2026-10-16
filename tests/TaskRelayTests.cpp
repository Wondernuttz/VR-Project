#include "Utils/TaskRelay.h"

#include <array>
#include <chrono>
#include <iostream>
#include <string>

namespace {
    using Relay = PrismaUI::Utils::TaskRelay;
    using Destination = Relay::Destination;
    using namespace std::chrono_literals;
    int failures = 0;

    void Expect(bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            ++failures;
        }
    }

    void TestRendererAndBrowserLockCycle() {
        std::timed_mutex renderer;
        std::mutex nativeQueueMutex;
        std::vector<std::pair<Destination, Relay::Task>> nativeQueue;
        std::promise<void> nativeQueueHeld, relayEntered;
        auto nativeQueueHeldFuture = nativeQueueHeld.get_future();
        auto relayEnteredFuture = relayEntered.get_future();
        std::atomic<bool> nativeTaskGotRenderer = false;
        std::atomic<int> submissionErrors = 0;
        std::vector<int> callbackOrder;
        bool wrongCallbackThread = false;
        std::thread::id callbackThread;
        const auto renderThread = std::this_thread::get_id();
        bool wrongSubmissionThread = false;
        std::unique_lock renderLock(renderer);

        // Match SKSE VR: its worker holds the task queue while a RaceMenu task
        // waits for the renderer. A timeout makes the old blocking path fail
        // this test cleanly instead of hanging the test process indefinitely.
        std::thread nativeWorker([&] {
            std::lock_guard queueLock(nativeQueueMutex);
            nativeQueueHeld.set_value();
            if (renderer.try_lock_for(3s)) {
                nativeTaskGotRenderer = true;
                renderer.unlock();
            }
        });
        nativeQueueHeldFuture.wait();

        {
            bool firstSubmission = true;
            Relay relay([&](Destination destination, Relay::Task task) {
                if (firstSubmission) {
                    firstSubmission = false;
                    relayEntered.set_value();
                }
                wrongSubmissionThread |= std::this_thread::get_id() == renderThread;
                std::lock_guard queueLock(nativeQueueMutex);
                nativeQueue.emplace_back(destination, std::move(task));
            }, [&](std::exception_ptr) { ++submissionErrors; });

            auto callback = [&](int value) {
                return [&, value] {
                    wrongCallbackThread |= std::this_thread::get_id() != callbackThread;
                    callbackOrder.push_back(value);
                };
            };
            const auto begin = std::chrono::steady_clock::now();
            relay.Post(Destination::Game, callback(0)); // combat mask
            Expect(std::chrono::steady_clock::now() - begin < 1s,
                   "render submission must not wait on the native task queue");
            Expect(relayEnteredFuture.wait_for(1s) == std::future_status::ready,
                   "relay must reach the blocked native queue");
            relay.Post(Destination::Game, callback(1)); // combat unmask

            // Rendering waits on browser work while still owning renderer.
            // The browser must be able to post while the relay is blocked.
            auto browser = std::async(std::launch::async, [&] {
                relay.Post(Destination::Game, callback(2));
                relay.Post(Destination::UI, callback(3));
            });
            Expect(browser.wait_for(1s) == std::future_status::ready,
                   "browser posting must finish while rendering owns its lock");
            Expect(callbackOrder.empty(), "relay must not execute game/UI callbacks itself");
            renderLock.unlock();
            browser.get();
            nativeWorker.join();
        } // Drain pending submissions only after renderer is released.

        Expect(nativeTaskGotRenderer, "native task must acquire renderer without timing out");
        Expect(submissionErrors == 0, "all queued callbacks must be submitted successfully");
        Expect(!wrongSubmissionThread, "native submission must happen off the render thread");
        Expect(nativeQueue.size() == 4, "all four callbacks must reach the native scheduler");
        if (nativeQueue.size() == 4) {
            Expect(nativeQueue[0].first == Destination::Game &&
                   nativeQueue[1].first == Destination::Game &&
                   nativeQueue[2].first == Destination::Game &&
                   nativeQueue[3].first == Destination::UI,
                   "game and UI destinations must be preserved");
        }
        // Emulate the native scheduler's execution context after submission.
        std::thread consumer([&] {
            callbackThread = std::this_thread::get_id();
            std::lock_guard queueLock(nativeQueueMutex);
            for (auto& [destination, task] : nativeQueue) task();
        });
        consumer.join();
        Expect(!wrongCallbackThread, "callback execution thread must be unchanged");
        Expect(callbackOrder == std::vector<int>({0, 1, 2, 3}),
               "mask, unmask and browser callbacks must retain submission order");
    }

    void TestConcurrentProducersAndOwnedPayloads() {
        constexpr int producerCount = 4;
        constexpr int tasksPerProducer = 1000;
        std::vector<Relay::Task> delivered;
        std::array<int, producerCount> next{};
        bool ordered = true;
        std::atomic<int> errors = 0;
        {
            Relay relay([&](Destination, Relay::Task task) {
                delivered.push_back(std::move(task));
            }, [&](std::exception_ptr) { ++errors; });
            std::vector<std::thread> producers;
            for (int producer = 0; producer < producerCount; ++producer) {
                producers.emplace_back([&, producer] {
                    for (int sequence = 0; sequence < tasksPerProducer; ++sequence) {
                        std::string payload = std::to_string(sequence);
                        relay.Post(Destination::Game, [&, producer, payload] {
                            ordered &= std::stoi(payload) == next[producer]++;
                        });
                    }
                });
            }
            for (auto& producer : producers) producer.join();
        }
        Expect(errors == 0, "concurrent posting must not lose submissions to errors");
        Expect(delivered.size() == producerCount * tasksPerProducer,
               "drain must deliver every queued callback");
        for (auto& callback : delivered) callback();
        Expect(ordered, "each producer's owned payloads must retain FIFO order");
        for (auto count : next) Expect(count == tasksPerProducer, "every producer must finish");
    }

    void TestSubmissionFailureIsReportedAndWorkerContinues() {
        int attempts = 0, errors = 0, callbacks = 0;
        std::vector<Relay::Task> delivered;
        {
            Relay relay([&](Destination, Relay::Task task) {
                if (++attempts == 1) throw std::runtime_error("simulated submission failure");
                delivered.push_back(std::move(task));
            }, [&](std::exception_ptr error) {
                try { std::rethrow_exception(error); }
                catch (const std::runtime_error&) { ++errors; }
            });
            relay.Post(Destination::Game, [&] { ++callbacks; });
            relay.Post(Destination::Game, [&] { ++callbacks; });
        }
        for (auto& task : delivered) task();
        Expect(attempts == 2 && errors == 1 && callbacks == 1,
               "failed submission must be reported and later work must still run");
    }
}

int main() {
    TestRendererAndBrowserLockCycle();
    TestConcurrentProducersAndOwnedPayloads();
    TestSubmissionFailureIsReportedAndWorkerContinues();
    if (failures) return 1;
    std::cout << "Task relay regression tests passed (lock cycle, browser, FIFO, destinations, ownership, errors).\n";
    return 0;
}
