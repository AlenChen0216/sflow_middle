#include "DeviceRuntime.hpp"

#include <chrono>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

namespace
{
    constexpr std::size_t kCurStateMinimumSize = sizeof(uint32_t);
    constexpr std::size_t kProcessingMeMinimumSize = 2 * sizeof(uint32_t);
    // MacClockReader supports the original packed 16-byte layout and the newer
    // 20-byte host layout.  Its constructor applies the same zero-size rule.
    constexpr std::size_t kMacClockMinimumSize = 16;
    constexpr auto kCollectionPeriod = std::chrono::seconds(1);

    class ProducerCompletionGuard
    {
    public:
        ProducerCompletionGuard(SharedQueue &shared, unsigned int devnum) noexcept
            : shared_(shared), devnum_(devnum)
        {
        }

        ~ProducerCompletionGuard() noexcept
        {
            try
            {
                shared_.producerFinished();
            }
            catch (const std::exception &ex)
            {
                SPDLOG_CRITICAL("[devnum={}] worker producer completion failed: {}",
                                devnum_, ex.what());
            }
            catch (...)
            {
                SPDLOG_CRITICAL("[devnum={}] worker producer completion failed: "
                                "unknown error",
                                devnum_);
            }
        }

        ProducerCompletionGuard(const ProducerCompletionGuard &) = delete;
        ProducerCompletionGuard &operator=(const ProducerCompletionGuard &) = delete;

    private:
        SharedQueue &shared_;
        unsigned int devnum_;
    };

    void addFlowEntries(const std::vector<flow_data> &entries,
                        TimeAdjuster &timeAdjuster,
                        std::map<Tuple, FlowRecord> &flowMap)
    {
        for (const auto &entry : entries)
        {
            Tuple key{};
            key.src_ip = entry.key.src_ip;
            key.dst_ip = entry.key.dst_ip;
            key.src_port = entry.key.dst_port;
            key.dst_port = entry.key.src_port;
            key.protocol = static_cast<uint16_t>(entry.key.protocol);

            Agent agent{};
            agent.aip = entry.key.agent_ip;
            agent.in_if = static_cast<uint32_t>(entry.key.out_if);
            agent.out_if = static_cast<uint32_t>(entry.key.in_if);
            agent.byte_cnt = entry.data.byte_cnt;
            agent.packet_cnt = entry.data.packet_cnt;
            agent.sampling_rate = entry.key.sampling_rate;
            agent.tcp_flag = static_cast<uint16_t>(entry.key.tcp_flag);
            agent.index = entry.index;

            const int64_t startTimeNs = timeAdjuster.toUnixNanoseconds(
                entry.data.start_time.sec, entry.data.start_time.nsec);
            const int64_t endTimeNs = timeAdjuster.toUnixNanoseconds(
                entry.data.end_time.sec, entry.data.end_time.nsec);

            FlowRecord &record = flowMap[key];
            record.agent.push_back(agent);
            record.start_time_ns = std::min(record.start_time_ns, startTimeNs);
            record.end_time_ns = std::max(record.end_time_ns, endTimeNs);
            record.start_time = record.start_time_ns / 1000000;
            record.end_time = record.end_time_ns / 1000000;
        }
    }

    void addCounterEntries(const std::vector<counter_data> &entries,
                           std::map<CounterAgent, CounterRecord> &counterMap)
    {
        for (const auto &entry : entries)
        {
            CounterAgent key{};
            key.agent_ip = entry.data.key.agent_ip;
            key.if_idx = entry.data.key.if_idx;

            CounterRecord record{};
            record.if_speed = entry.data.if_speed;
            record.in_octets = entry.data.in_octets;
            record.out_octets = entry.data.out_octets;
            counterMap[key] = record;
        }
    }

    const nfp_rtsym *resolveSymbol(const NFPDevice &device,
                                   unsigned int devnum,
                                   const std::string &name,
                                   std::size_t minimumSize)
    {
        const nfp_rtsym *symbol = device.getSymbolData(name.c_str());
        if (!symbol)
        {
            throw std::runtime_error("[devnum=" + std::to_string(devnum) +
                                     "] required symbol '" + name +
                                     "' is missing");
        }

        // Some SDK versions leave rtsym::size at zero for otherwise valid
        // runtime symbols.  Preserve that compatibility behavior while checking
        // every known size strictly.
        if (symbol->size != 0 && symbol->size < minimumSize)
        {
            std::ostringstream message;
            message << "[devnum=" << devnum << "] symbol '" << name
                    << "' is too small: size=" << symbol->size
                    << ", required at least " << minimumSize;
            throw std::runtime_error(message.str());
        }

        SPDLOG_INFO("[devnum={}] symbol {}: addr=0x{:x}, domain={}, size={}",
                    devnum, name, symbol->addr, symbol->domain, symbol->size);
        return symbol;
    }
} // namespace

RuntimeSymbols resolveRuntimeSymbols(const NFPDevice &device,
                                     unsigned int devnum,
                                     const AppConfig &config)
{
    const SymbolConfig &names = config.symbols;
    RuntimeSymbols symbols;

    symbols.semaphore = resolveSymbol(device, devnum, names.semaphore,
                                      kFlowSlotCount * sizeof(uint32_t));
    symbols.semaphore_dup = resolveSymbol(device, devnum, names.semaphore_dup,
                                          kFlowSlotCount * sizeof(uint32_t));
    symbols.flow_key = resolveSymbol(device, devnum, names.flow_key,
                                     kFlowSlotCount * sizeof(stored_flow_key));
    symbols.flow_key_dup = resolveSymbol(device, devnum, names.flow_key_dup,
                                         kFlowSlotCount * sizeof(stored_flow_key));
    symbols.flow_data = resolveSymbol(device, devnum, names.flow_data,
                                      kFlowSlotCount * sizeof(stored_flow_data));
    symbols.flow_data_dup = resolveSymbol(device, devnum, names.flow_data_dup,
                                          kFlowSlotCount * sizeof(stored_flow_data));
    symbols.counter_semaphore = resolveSymbol(
        device, devnum, names.counter_semaphore,
        kCounterSlotCount * sizeof(uint32_t));
    symbols.counter_semaphore_dup = resolveSymbol(
        device, devnum, names.counter_semaphore_dup,
        kCounterSlotCount * sizeof(uint32_t));
    symbols.counter_data = resolveSymbol(device, devnum, names.counter_data,
                                         kCounterSlotCount * sizeof(stored_counter));
    symbols.counter_data_dup = resolveSymbol(
        device, devnum, names.counter_data_dup,
        kCounterSlotCount * sizeof(stored_counter));
    symbols.cur_state = resolveSymbol(device, devnum, names.cur_state,
                                      kCurStateMinimumSize);
    symbols.processing_me = resolveSymbol(device, devnum, names.processing_me,
                                          kProcessingMeMinimumSize);
    symbols.mac_time = resolveSymbol(device, devnum, config.time.mac_time_symbol,
                                     kMacClockMinimumSize);
    return symbols;
}

DeviceRuntime::DeviceRuntime(unsigned int devnum, const AppConfig &config)
    : devnum_(devnum)
{
    try
    {
        device_ = std::make_unique<NFPDevice>(devnum_);
        symbols_ = resolveRuntimeSymbols(*device_, devnum_, config);

        flow_reader_ = std::make_unique<SmartNicReader>(
            device_->cpp(),
            symbols_.cur_state->addr, symbols_.cur_state->domain,
            symbols_.processing_me->addr, symbols_.processing_me->domain,
            symbols_.semaphore->addr, symbols_.semaphore_dup->addr,
            symbols_.flow_key->addr, symbols_.flow_key_dup->addr,
            symbols_.flow_data->addr, symbols_.flow_data_dup->addr,
            symbols_.semaphore->domain, symbols_.flow_key->domain,
            symbols_.flow_data->domain, symbols_.semaphore_dup->domain,
            symbols_.flow_key_dup->domain, symbols_.flow_data_dup->domain,
            kFlowSlotCount);

        counter_reader_ = std::make_unique<CounterReader>(
            device_->cpp(),
            symbols_.cur_state->addr, symbols_.cur_state->domain,
            symbols_.processing_me->addr, symbols_.processing_me->domain,
            symbols_.counter_semaphore->addr,
            symbols_.counter_semaphore_dup->addr,
            symbols_.counter_data->addr, symbols_.counter_data_dup->addr,
            symbols_.counter_semaphore->domain, symbols_.counter_data->domain,
            symbols_.counter_semaphore_dup->domain,
            symbols_.counter_data_dup->domain, kCounterSlotCount);

        time_adjuster_ = std::make_unique<TimeAdjuster>(
            device_->cpp(), symbols_.mac_time, config.time, devnum_);
        const CalibrationStatus calibration = time_adjuster_->status();
        SPDLOG_INFO("[devnum={}] SmartNIC clock calibrated: minimum latency "
                    "{} us, samples {}",
                    devnum_, calibration.minimum_latency_ns / 1000,
                    calibration.sample_count);
    }
    catch (const std::exception &ex)
    {
        throw std::runtime_error("[devnum=" + std::to_string(devnum_) +
                                 "] runtime initialization failed: " +
                                 ex.what());
    }
    catch (...)
    {
        throw std::runtime_error("[devnum=" + std::to_string(devnum_) +
                                 "] runtime initialization failed: unknown error");
    }
}

void DeviceRuntime::startWorker(SharedQueue &shared, ShutdownState &shutdown)
{
    if (worker_.joinable())
    {
        throw std::logic_error("[devnum=" + std::to_string(devnum_) +
                               "] worker is already running");
    }
    worker_ = std::thread(&DeviceRuntime::runWorker, this,
                          std::ref(shared), std::ref(shutdown));
}

void DeviceRuntime::joinWorker()
{
    if (worker_.joinable())
    {
        worker_.join();
    }
}

void DeviceRuntime::runWorker(SharedQueue &shared, ShutdownState &shutdown) noexcept
{
    ProducerCompletionGuard completion(shared, devnum_);
    try
    {
        auto nextTick = std::chrono::steady_clock::now();
        bool driftCalibrationReported = false;

        while (!shutdown.stopRequested())
        {
            nextTick += kCollectionPeriod;
            std::this_thread::sleep_until(nextTick);

            const auto start = std::chrono::steady_clock::now();
            // Flow must be read first: it swaps the shared firmware buffer
            // which the counter reader then consumes for this snapshot.
            const std::vector<flow_data> flowEntries =
                flow_reader_->readFlowData();
            const std::vector<counter_data> counterEntries =
                counter_reader_->readCounterData();

            if (!time_adjuster_->refresh())
            {
                SPDLOG_WARN("[devnum={}] SmartNIC clock refresh sample rejected; "
                            "continuing with the previous calibration",
                            devnum_);
            }
            const CalibrationStatus calibration = time_adjuster_->status();
            if (!calibration.drift_calibrated)
            {
                driftCalibrationReported = false;
            }
            else if (!driftCalibrationReported)
            {
                SPDLOG_INFO("[devnum={}] SmartNIC clock drift calibrated: {} ppm, "
                            "median residual {} us, samples {}",
                            devnum_,
                            (calibration.scale - 1.0) * 1000000.0,
                            calibration.median_residual_ns / 1000,
                            calibration.sample_count);
                driftCalibrationReported = true;
            }

            const auto end = std::chrono::steady_clock::now();
            const std::chrono::duration<double> elapsed = end - start;
            SPDLOG_INFO("[devnum={}] readFlowData() took {} seconds and returned {} entries",
                        devnum_, elapsed.count(), flowEntries.size());

            std::map<Tuple, FlowRecord> flowMap;
            std::map<CounterAgent, CounterRecord> counterMap;
            addFlowEntries(flowEntries, *time_adjuster_, flowMap);
            addCounterEntries(counterEntries, counterMap);

            if (!flowMap.empty() || !counterMap.empty())
            {
                ExportData batch;
                batch.devnum = devnum_;
                batch.execution_time = elapsed.count();
                batch.flow_map = std::move(flowMap);
                batch.counter_map = std::move(counterMap);
                shared.push(std::move(batch));
            }
        }
        SPDLOG_INFO("[devnum={}] worker exiting", devnum_);
    }
    catch (const std::exception &ex)
    {
        SPDLOG_ERROR("[devnum={}] worker failed: {}", devnum_, ex.what());
        shutdown.requestStop(true);
        shared.notifyAll();
    }
    catch (...)
    {
        SPDLOG_ERROR("[devnum={}] worker failed: unknown exception", devnum_);
        shutdown.requestStop(true);
        shared.notifyAll();
    }
}
