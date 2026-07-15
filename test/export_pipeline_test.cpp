#include "ExportPipeline.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
class ProducerCompletion
{
public:
    explicit ProducerCompletion(SharedQueue &queue) : queue_(queue) {}

    ~ProducerCompletion() noexcept
    {
        try {
            queue_.producerFinished();
        } catch (...) {
            std::terminate();
        }
    }

    ProducerCompletion(const ProducerCompletion &) = delete;
    ProducerCompletion &operator=(const ProducerCompletion &) = delete;

private:
    SharedQueue &queue_;
};

void require(bool condition, const std::string &message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

Tuple flowKey()
{
    Tuple key{};
    key.src_ip = 0x0a000001;
    key.dst_ip = 0x0a000002;
    key.src_port = 1234;
    key.dst_port = 6343;
    key.protocol = 17;
    return key;
}

FlowRecord flowRecord()
{
    FlowRecord record;
    record.start_time = 1700000000123;
    record.end_time = 1700000000456;
    record.start_time_ns = record.start_time * 1000000;
    record.end_time_ns = record.end_time * 1000000;
    record.agent.push_back({0x0a0000fe, 3, 4, 99, 7, 1024, 0x12, 42});
    return record;
}

CounterAgent counterKey()
{
    return {0x0a0000fe, 8};
}

CounterRecord counterRecord()
{
    return {std::numeric_limits<std::uint64_t>::max() - 1,
            std::numeric_limits<std::uint64_t>::max(),
            100000000000ULL};
}

ExportData batch(unsigned int devnum, unsigned int sequence)
{
    ExportData data;
    data.devnum = devnum;
    data.execution_time = static_cast<double>(sequence);
    return data;
}

void testOneRecordPayloadsKeepFieldsAndDevice()
{
    const auto flow = makeFlowPayload(2, flowKey(), flowRecord());
    require(flow.at("devnum") == 2, "flow payload lost devnum");
    require(flow.at("flowMap").size() == 1, "flow payload must contain one record");
    const auto &flowItem = flow.at("flowMap").at(0);
    require(flowItem.at("src_ip") == 0x0a000001,
            "flow payload lost source address");
    require(flowItem.at("start_time") == 1700000000123,
            "flow timestamp unit changed");
    require(flowItem.at("agent").at(0).at("byte_cnt") == 99,
            "flow agent fields changed");

    const auto counter = makeCounterPayload(7, counterKey(), counterRecord());
    require(counter.at("devnum") == 7, "counter payload lost devnum");
    require(counter.at("counterMap").size() == 1,
            "counter payload must contain one record");
    require(counter.at("counterMap").at(0).at("in_octets") ==
                std::numeric_limits<std::uint64_t>::max() - 1,
            "large unsigned counter value changed");
    require(counter.at("counterMap").at(0).at("out_octets") ==
                std::numeric_limits<std::uint64_t>::max(),
            "maximum unsigned counter value changed");
}

void testDebugPayloadsPreserveBatchBoundaries()
{
    ExportData first;
    first.devnum = 0;
    first.execution_time = 0.125;
    first.flow_map.emplace(flowKey(), flowRecord());

    ExportData second;
    second.devnum = 1;
    second.execution_time = 0.25;
    second.counter_map.emplace(counterKey(), counterRecord());

    const auto firstJson = makeDebugPayload(first);
    const auto secondJson = makeDebugPayload(second);
    require(firstJson.at("devnum") == 0 && secondJson.at("devnum") == 1,
            "debug batches use the wrong device IDs");
    require(firstJson.at("flowMap").size() == 1 &&
                firstJson.at("counterMap").empty(),
            "first debug batch combined records from another device");
    require(secondJson.at("flowMap").empty() &&
                secondJson.at("counterMap").size() == 1,
            "second debug batch combined records from another device");
    require(firstJson.at("execution_time") == 0.125,
            "debug execution time changed");
}

void testDebugPayloadKeepsEmptyCollectionsAsArrays()
{
    ExportData batch;
    batch.devnum = 99;
    const auto payload = makeDebugPayload(batch);
    require(payload.at("devnum") == 99, "empty debug payload lost devnum");
    require(payload.at("flowMap").is_array() && payload.at("flowMap").empty(),
            "empty flow collection is not an array");
    require(payload.at("counterMap").is_array() && payload.at("counterMap").empty(),
            "empty counter collection is not an array");
}

void testConcurrentProducersPreserveEveryBatch()
{
    constexpr unsigned int producerCount = 4;
    constexpr unsigned int batchesPerProducer = 32;
    SharedQueue queue(producerCount);
    std::atomic<bool> start{false};
    std::vector<std::thread> producers;

    for (unsigned int devnum = 0; devnum < producerCount; ++devnum) {
        producers.emplace_back([&, devnum] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (unsigned int sequence = 0; sequence < batchesPerProducer; ++sequence) {
                queue.push(batch(devnum, sequence));
            }
            queue.producerFinished();
        });
    }

    start.store(true, std::memory_order_release);
    for (auto &producer : producers) {
        producer.join();
    }

    std::vector<std::vector<bool>> seen(
        producerCount, std::vector<bool>(batchesPerProducer, false));
    ExportData data;
    std::size_t received = 0;
    while (queue.waitPop(data)) {
        const auto sequence = static_cast<unsigned int>(data.execution_time);
        require(data.devnum < producerCount && sequence < batchesPerProducer,
                "queue returned an unexpected device-tagged batch");
        require(!seen[data.devnum][sequence], "queue duplicated a batch");
        seen[data.devnum][sequence] = true;
        ++received;
    }

    require(received == producerCount * batchesPerProducer,
            "queue lost batches from concurrent producers");
}

void testConsumerRemainsActiveUntilLastProducerFinishes()
{
    using namespace std::chrono_literals;

    SharedQueue queue(2);
    queue.producerFinished();

    std::promise<ExportData> receivedPromise;
    auto received = receivedPromise.get_future();
    std::thread consumer([&] {
        try {
            ExportData data;
            require(queue.waitPop(data),
                    "consumer ended while a producer remained active");
            receivedPromise.set_value(std::move(data));
        } catch (...) {
            receivedPromise.set_exception(std::current_exception());
        }
    });

    require(received.wait_for(100ms) == std::future_status::timeout,
            "consumer did not block while a producer remained active");
    queue.notifyAll();
    require(received.wait_for(100ms) == std::future_status::timeout,
            "notifyAll must not end the consumer while a producer remains active");

    queue.push(batch(7, 11));
    if (received.wait_for(500ms) != std::future_status::ready) {
        queue.producerFinished();
        consumer.join();
        throw std::runtime_error(
            "consumer did not receive a batch from the remaining producer");
    }
    const auto data = received.get();
    consumer.join();
    require(data.devnum == 7 && data.execution_time == 11.0,
            "consumer received the wrong batch after early producer completion");

    queue.producerFinished();
    ExportData ignored;
    const auto start = std::chrono::steady_clock::now();
    require(!queue.waitPop(ignored), "consumer did not terminate after all producers finished");
    require(std::chrono::steady_clock::now() - start < 100ms,
            "empty queue termination was not prompt");
}

void testQueuedBatchesDrainBeforeCompletionAndUnderflowIsGuarded()
{
    SharedQueue queue(2);
    queue.push(batch(3, 1));
    queue.push(batch(4, 2));
    queue.producerFinished();
    queue.producerFinished();

    ExportData first;
    ExportData second;
    ExportData ignored;
    require(queue.waitPop(first) && queue.waitPop(second),
            "queued batches did not drain before completion");
    require(first.devnum == 3 && second.devnum == 4,
            "queued batches were not returned intact and in order");
    require(!queue.waitPop(ignored), "queue reported data after draining");

    bool underflowGuarded = false;
    try {
        queue.producerFinished();
    } catch (const std::logic_error &) {
        underflowGuarded = true;
    }
    require(underflowGuarded, "producer count underflow was not guarded");
}

void testWorkerExceptionRequestsFailureAndCompletesProducer()
{
    using namespace std::chrono_literals;

    SharedQueue queue(1);
    ShutdownState shutdown;
    std::promise<bool> consumerResult;
    auto consumer = consumerResult.get_future();
    std::thread transmitter([&] {
        try {
            ExportData ignored;
            consumerResult.set_value(queue.waitPop(ignored));
        } catch (...) {
            consumerResult.set_exception(std::current_exception());
        }
    });

    std::thread worker([&] {
        ProducerCompletion completion(queue);
        try {
            throw std::runtime_error("simulated worker failure");
        } catch (...) {
            shutdown.requestStop(true);
            queue.notifyAll();
        }
    });
    worker.join();

    if (consumer.wait_for(500ms) != std::future_status::ready) {
        // Keep failure reporting bounded without leaving a consumer blocked.
        // If the guard did run, this harmlessly trips the underflow guard.
        try {
            queue.producerFinished();
        } catch (const std::logic_error &) {
        }
        queue.notifyAll();
        transmitter.join();
        throw std::runtime_error(
            "worker exception did not release the producer completion obligation");
    }
    require(!consumer.get(), "empty queue returned data after worker exception");
    transmitter.join();
    require(shutdown.stopRequested() && shutdown.hasFailed(),
            "worker exception did not request failed shutdown");
}

void testThreadLaunchFailureReleasesNeverStartedProducerObligations()
{
    using namespace std::chrono_literals;

    constexpr std::size_t runtimeCount = 4;
    SharedQueue queue(runtimeCount);
    ShutdownState shutdown;
    std::promise<bool> consumerResult;
    auto consumer = consumerResult.get_future();
    std::thread transmitter([&] {
        try {
            ExportData ignored;
            consumerResult.set_value(queue.waitPop(ignored));
        } catch (...) {
            consumerResult.set_exception(std::current_exception());
        }
    });

    // Model one already-started worker and a failure while launching the next.
    // The three remaining obligations represent the failed and never-started workers.
    std::thread startedWorker([&] {
        ProducerCompletion completion(queue);
        shutdown.waitUntil(std::chrono::steady_clock::now() + 1h);
    });
    shutdown.requestStop(true);
    queue.notifyAll();
    for (std::size_t index = 1; index < runtimeCount; ++index) {
        queue.producerFinished();
    }
    startedWorker.join();

    if (consumer.wait_for(500ms) != std::future_status::ready) {
        try {
            queue.producerFinished();
        } catch (const std::logic_error &) {
        }
        queue.notifyAll();
        transmitter.join();
        throw std::runtime_error(
            "simulated thread-launch failure left producer obligations active");
    }
    require(!consumer.get(), "empty queue returned data after thread-launch failure");
    transmitter.join();
    require(shutdown.stopRequested() && shutdown.hasFailed(),
            "thread-launch failure did not request failed shutdown");
}

void testShutdownInterruptsScheduledWait()
{
    using namespace std::chrono_literals;

    ShutdownState shutdown;
    std::promise<std::chrono::steady_clock::duration> elapsedPromise;
    auto elapsed = elapsedPromise.get_future();
    std::thread worker([&] {
        const auto start = std::chrono::steady_clock::now();
        const bool interrupted = shutdown.waitUntil(start + 1s);
        try {
            require(interrupted, "scheduled wait ended without observing stop");
            elapsedPromise.set_value(std::chrono::steady_clock::now() - start);
        } catch (...) {
            elapsedPromise.set_exception(std::current_exception());
        }
    });

    std::this_thread::sleep_for(25ms);
    shutdown.requestStop(false);
    if (elapsed.wait_for(500ms) != std::future_status::ready) {
        worker.join();
        throw std::runtime_error("stop request did not wake scheduled wait promptly");
    }
    const auto duration = elapsed.get();
    worker.join();
    require(duration < 500ms, "stop request took too long to wake scheduled wait");
    require(shutdown.stopRequested(), "normal stop was not recorded");
    require(!shutdown.hasFailed(), "normal stop incorrectly recorded failure");
}

void testShutdownFailureIsStickyAndRequestsAreIdempotent()
{
    ShutdownState shutdown;
    shutdown.requestStop(false);
    shutdown.requestStop(false);
    require(shutdown.stopRequested(), "repeated normal stop did not record stop");
    require(!shutdown.hasFailed(), "normal stop incorrectly recorded failure");

    shutdown.requestStop(true);
    require(shutdown.hasFailed(), "failed stop did not record failure");
    shutdown.requestStop(false);
    require(shutdown.stopRequested(), "later normal stop cleared stop state");
    require(shutdown.hasFailed(), "later normal stop cleared failure state");
}

void testDeadlineWaitExpiresWithoutStopping()
{
    using namespace std::chrono_literals;

    ShutdownState shutdown;
    const auto start = std::chrono::steady_clock::now();
    require(!shutdown.waitUntil(start + 10ms),
            "deadline wait incorrectly reported an unrequested stop");
    require(!shutdown.stopRequested() && !shutdown.hasFailed(),
            "deadline expiration changed shutdown state");
}
} // namespace

int main()
{
    try {
        testOneRecordPayloadsKeepFieldsAndDevice();
        testDebugPayloadsPreserveBatchBoundaries();
        testDebugPayloadKeepsEmptyCollectionsAsArrays();
        testConcurrentProducersPreserveEveryBatch();
        testConsumerRemainsActiveUntilLastProducerFinishes();
        testQueuedBatchesDrainBeforeCompletionAndUnderflowIsGuarded();
        testWorkerExceptionRequestsFailureAndCompletesProducer();
        testThreadLaunchFailureReleasesNeverStartedProducerObligations();
        testShutdownInterruptsScheduledWait();
        testShutdownFailureIsStickyAndRequestsAreIdempotent();
        testDeadlineWaitExpiresWithoutStopping();
    } catch (const std::exception &ex) {
        std::cerr << "export pipeline test failed: " << ex.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "export pipeline tests passed\n";
    return EXIT_SUCCESS;
}
