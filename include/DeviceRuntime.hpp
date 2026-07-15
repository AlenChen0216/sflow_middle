#pragma once

#include "AppConfig.hpp"
#include "CounterReader.hpp"
#include "Device.hpp"
#include "ExportPipeline.hpp"
#include "Reader.hpp"

#include <memory>
#include <thread>

extern "C" {
#include <nfp_nffw.h>
}

// The firmware symbols are resolved for one NFPDevice at a time.  This
// aggregate deliberately stores no global state: each DeviceRuntime created
// by TASK-007 will own a distinct instance.
struct RuntimeSymbols
{
    const nfp_rtsym *semaphore = nullptr;
    const nfp_rtsym *semaphore_dup = nullptr;
    const nfp_rtsym *flow_key = nullptr;
    const nfp_rtsym *flow_key_dup = nullptr;
    const nfp_rtsym *flow_data = nullptr;
    const nfp_rtsym *flow_data_dup = nullptr;
    const nfp_rtsym *counter_semaphore = nullptr;
    const nfp_rtsym *counter_semaphore_dup = nullptr;
    const nfp_rtsym *counter_data = nullptr;
    const nfp_rtsym *counter_data_dup = nullptr;
    const nfp_rtsym *cur_state = nullptr;
    const nfp_rtsym *processing_me = nullptr;
    const nfp_rtsym *mac_time = nullptr;
};

// Resolves and validates all firmware symbols against exactly the supplied
// device.  Throws std::runtime_error with both "devnum" and the configured
// symbol name whenever a symbol is missing or its known size is incompatible.
// A reported size of zero is an SDK "unknown size" value and skips only the
// corresponding size check.
RuntimeSymbols resolveRuntimeSymbols(const NFPDevice &device,
                                     unsigned int devnum,
                                     const AppConfig &config);

// Owns every resource derived from one SmartNIC. The declaration order is
// intentional: readers and the clock model are destroyed before their device
// handle is closed.
class DeviceRuntime
{
public:
    DeviceRuntime(unsigned int devnum, const AppConfig &config);
    ~DeviceRuntime() = default;

    DeviceRuntime(const DeviceRuntime &) = delete;
    DeviceRuntime &operator=(const DeviceRuntime &) = delete;
    DeviceRuntime(DeviceRuntime &&) = delete;
    DeviceRuntime &operator=(DeviceRuntime &&) = delete;

    unsigned int devnum() const noexcept { return devnum_; }
    NFPDevice &device() noexcept { return *device_; }
    const NFPDevice &device() const noexcept { return *device_; }
    const RuntimeSymbols &symbols() const noexcept { return symbols_; }
    SmartNicReader &flowReader() noexcept { return *flow_reader_; }
    CounterReader &counterReader() noexcept { return *counter_reader_; }
    TimeAdjuster &timeAdjuster() noexcept { return *time_adjuster_; }

    // Starts this runtime's sole producer.  The caller must arrange for one
    // SharedQueue producer obligation per runtime before any worker starts.
    void startWorker(SharedQueue &shared, ShutdownState &shutdown);
    void joinWorker();
    bool workerJoinable() const noexcept { return worker_.joinable(); }

private:
    void runWorker(SharedQueue &shared, ShutdownState &shutdown) noexcept;

    unsigned int devnum_;
    std::unique_ptr<NFPDevice> device_;
    RuntimeSymbols symbols_;
    std::unique_ptr<SmartNicReader> flow_reader_;
    std::unique_ptr<CounterReader> counter_reader_;
    std::unique_ptr<TimeAdjuster> time_adjuster_;
    std::thread worker_;
};
