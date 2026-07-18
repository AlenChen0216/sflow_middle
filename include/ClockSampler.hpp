#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

constexpr int64_t kNanosecondsPerSecond = 1000000000LL;

struct HostClockBounds
{
    int64_t realtime_before_ns = 0;
    int64_t realtime_after_ns = 0;
    int64_t monotonic_before_ns = 0;
    int64_t monotonic_after_ns = 0;
};

struct MacTimeStateHost
{
    uint32_t mac_time_s = 0;
    uint32_t mac_time_ns = 0;
    uint32_t me_time = 0;
    uint32_t conv_mult = 0;
    uint32_t conv_rshift = 0;

    bool operator==(const MacTimeStateHost &other) const;
    bool operator!=(const MacTimeStateHost &other) const;
};

struct MacStateObservation
{
    MacTimeStateHost state;
    HostClockBounds bounds;
    int64_t read_latency_ns = 0;
    bool valid = false;
    std::string reject_reason;
};

struct PublicationSample
{
    int64_t smartnic_ns = 0;
    int64_t host_realtime_ns = 0;
    int64_t host_monotonic_ns = 0;
    int64_t uncertainty_ns = 0;
    int64_t transition_start_mono_ns = 0;
    int64_t transition_end_mono_ns = 0;
    uint32_t me_time = 0;
    uint32_t conv_mult = 0;
    uint32_t conv_rshift = 0;
};

class IClockStateReader
{
public:
    virtual ~IClockStateReader() = default;
    virtual MacStateObservation readState() = 0;
};

struct ClockSamplerCounters
{
    std::size_t observations = 0;
    std::size_t accepted_transitions = 0;
    std::size_t duplicate_states = 0;
    std::size_t invalid_observations = 0;
    std::size_t rejected_transitions = 0;
    std::size_t host_clock_steps = 0;
    std::size_t device_resets = 0;
};

class ClockSampler
{
public:
    using ClockFn = std::function<int64_t()>;

    ClockSampler(IClockStateReader &reader,
                 ClockFn realtimeClock,
                 ClockFn monotonicClock,
                 int64_t maximumUncertaintyNs);

    std::optional<PublicationSample> poll();
    ClockSamplerCounters counters() const;
    void reset();

private:
    static bool stateTimeNs(const MacTimeStateHost &state, int64_t &timeNs);
    static int64_t midpoint(int64_t first, int64_t second);

    bool isPlausibleTransition(const MacTimeStateHost &previous,
                               const MacTimeStateHost &current,
                               std::string &reason) const;

    IClockStateReader &reader_;
    ClockFn realtimeClock_;
    ClockFn monotonicClock_;
    int64_t maximumUncertaintyNs_;
    std::optional<MacStateObservation> lastObservation_;
    int64_t previousRealtimeMidpointNs_ = 0;
    int64_t previousMonotonicMidpointNs_ = 0;
    ClockSamplerCounters counters_;
};
