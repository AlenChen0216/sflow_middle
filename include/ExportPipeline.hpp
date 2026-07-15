#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <queue>
#include <thread>

#include <nlohmann/json.hpp>

#include "CounterReader.hpp"
#include "Reader.hpp"

// A queue item is a complete snapshot from exactly one SmartNIC.  Device
// identity intentionally lives at this boundary instead of in the map keys.
struct ExportData
{
    unsigned int devnum = 0;
    double execution_time = 0.0;
    std::map<Tuple, FlowRecord> flow_map;
    std::map<CounterAgent, CounterRecord> counter_map;
};

// Process-wide shutdown coordination.  A stop request is independent from
// whether the process should report failure: signals and runtime deadlines are
// normal stops, while setup and background-worker failures are not.
class ShutdownState
{
public:
    // Safe to call from every shutdown initiator.  Once failure is recorded it
    // remains recorded, even if a later caller requests a normal stop.
    void requestStop(bool failed = false) noexcept;

    bool stopRequested() const noexcept;
    bool hasFailed() const noexcept;

    // Returns true when stopped before the deadline and false when the
    // deadline expires normally.  This serves both per-worker tick waits and
    // the application's optional runtime deadline wait.
    bool waitUntil(std::chrono::steady_clock::time_point deadline);

private:
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> failed_{false};
    std::mutex wait_mutex_;
    std::condition_variable wait_cv_;
};

class SharedQueue
{
public:
    explicit SharedQueue(std::size_t producer_count);

    void push(ExportData data);
    bool waitPop(ExportData &data);
    void producerFinished();
    void notifyAll();

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<ExportData> queue_;
    std::size_t active_producers_;
};

nlohmann::json makeFlowPayload(unsigned int devnum,
                               const Tuple &key,
                               const FlowRecord &record);

nlohmann::json makeCounterPayload(unsigned int devnum,
                                  const CounterAgent &key,
                                  const CounterRecord &record);

nlohmann::json makeDebugPayload(const ExportData &batch);
