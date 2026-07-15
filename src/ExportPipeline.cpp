#include "ExportPipeline.hpp"

#include <stdexcept>

namespace
{
nlohmann::json makeFlowRecord(const Tuple &key, const FlowRecord &record)
{
    nlohmann::json item;
    item["src_ip"] = key.src_ip;
    item["dst_ip"] = key.dst_ip;
    item["src_port"] = key.src_port;
    item["dst_port"] = key.dst_port;
    item["protocol"] = key.protocol;
    item["start_time"] = record.start_time;
    item["end_time"] = record.end_time;

    nlohmann::json agents = nlohmann::json::array();
    for (const auto &agent : record.agent) {
        agents.push_back({{"aip", agent.aip},
                          {"in_if", agent.in_if},
                          {"out_if", agent.out_if},
                          {"byte_cnt", agent.byte_cnt},
                          {"packet_cnt", agent.packet_cnt},
                          {"sampling_rate", agent.sampling_rate},
                          {"tcp_flag", agent.tcp_flag},
                          {"index", agent.index}});
    }
    item["agent"] = std::move(agents);
    return item;
}

nlohmann::json makeCounterRecord(const CounterAgent &key,
                                 const CounterRecord &record)
{
    return {{"agent_ip", key.agent_ip},
            {"if_idx", key.if_idx},
            {"if_speed", record.if_speed},
            {"in_octets", record.in_octets},
            {"out_octets", record.out_octets}};
}
} // namespace

void ShutdownState::requestStop(bool failed) noexcept
{
    if (failed) {
        failed_.store(true, std::memory_order_release);
    }
    stop_requested_.store(true, std::memory_order_release);

    // Synchronize with waitUntil() so a stop request cannot be missed between
    // its predicate check and entering the condition-variable wait.
    {
        std::lock_guard<std::mutex> lock(wait_mutex_);
    }
    wait_cv_.notify_all();
}

bool ShutdownState::stopRequested() const noexcept
{
    return stop_requested_.load(std::memory_order_acquire);
}

bool ShutdownState::hasFailed() const noexcept
{
    return failed_.load(std::memory_order_acquire);
}


SharedQueue::SharedQueue(std::size_t producer_count)
    : active_producers_(producer_count)
{
}

void SharedQueue::push(ExportData data)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(data));
    }
    cv_.notify_one();
}

bool SharedQueue::waitPop(ExportData &data)
{
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] {
        return !queue_.empty() || active_producers_ == 0;
    });

    if (queue_.empty()) {
        return false;
    }

    data = std::move(queue_.front());
    queue_.pop();
    return true;
}

void SharedQueue::producerFinished()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_producers_ == 0) {
            throw std::logic_error("SharedQueue producer count underflow");
        }
        --active_producers_;
    }
    cv_.notify_all();
}

void SharedQueue::notifyAll()
{
    cv_.notify_all();
}

nlohmann::json makeFlowPayload(unsigned int devnum,
                               const Tuple &key,
                               const FlowRecord &record)
{
    return {{"devnum", devnum},
            {"flowMap", nlohmann::json::array({makeFlowRecord(key, record)})}};
}

nlohmann::json makeCounterPayload(unsigned int devnum,
                                  const CounterAgent &key,
                                  const CounterRecord &record)
{
    return {{"devnum", devnum},
            {"counterMap", nlohmann::json::array({makeCounterRecord(key, record)})}};
}

nlohmann::json makeDebugPayload(const ExportData &batch)
{
    nlohmann::json flows = nlohmann::json::array();
    for (const auto &[key, record] : batch.flow_map) {
        flows.push_back(makeFlowRecord(key, record));
    }

    nlohmann::json counters = nlohmann::json::array();
    for (const auto &[key, record] : batch.counter_map) {
        counters.push_back(makeCounterRecord(key, record));
    }

    return {{"devnum", batch.devnum},
            {"execution_time", batch.execution_time},
            {"flowMap", std::move(flows)},
            {"counterMap", std::move(counters)}};
}
