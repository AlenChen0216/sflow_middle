#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

extern "C" {
#include <nfp.h>
#include <nfp_cpp.h>
}

struct mac_time_state {
    uint32_t mac_time_s; /* last synced mac time seconds */
    uint32_t mac_time_ns; /* last synced mac time nanoseconds */
    uint32_t me_time; /* me cycle time at last sync */
    uint16_t conv_mult; /* multiplier to convert me time to ns */
    uint16_t conv_rshift; /* right shift to convert me time to ns */
};

struct TimeSample
{
    int64_t mac_ns = 0;
    int64_t unix_ns = 0;
    int64_t monotonic_ns = 0;
    int64_t latency_ns = 0;
};

struct CalibrationStatus
{
    int64_t mac_origin_ns = 0;
    int64_t unix_origin_ns = 0;
    int64_t minimum_latency_ns = 0;
    int64_t median_residual_ns = 0;
    double scale = 1.0;
    std::size_t sample_count = 0;
    std::size_t rejected_samples = 0;
    bool valid = false;
    bool drift_calibrated = false;
};

bool composeAtomicMacTime(uint32_t secondsBefore,
                          uint32_t nanoseconds,
                          uint32_t secondsAfter,
                          int64_t &macTimeNs);

class ClockCalibration
{
public:
    static constexpr int64_t kMaximumSampleLatencyNs = 800000;
    static constexpr int64_t kRegressionWindowNs = 120000000000LL;
    static constexpr int64_t kMinimumRegressionSpanNs = 5000000000LL;

    bool initialize(const std::vector<TimeSample> &samples);
    bool addSample(const TimeSample &sample);
    int64_t toUnixNanoseconds(int64_t mac_ns) const;
    CalibrationStatus status() const;
    void reset();

private:
    bool fit();
    void setOffsetModel(const std::vector<TimeSample> &samples);

    std::deque<TimeSample> samples_;
    CalibrationStatus status_;
    int64_t last_mac_ns_ = 0;
    int64_t last_unix_ns_ = 0;
    int64_t last_monotonic_ns_ = 0;
};

class TimeAdjuster
{
public:
    static constexpr uint32_t kDefaultMacClockXpbAddress = 0x0840001c;

    explicit TimeAdjuster(
        nfp_cpp *cpp,
        uint32_t macClockXpbAddress = kDefaultMacClockXpbAddress);

    bool refresh();
    int64_t toUnixNanoseconds(uint32_t sec, uint32_t nsec) const;
    CalibrationStatus status() const;

private:
    bool readSample(TimeSample &sample) const;
    std::vector<TimeSample> collectSamples(
        std::size_t count,
        bool pauseBetweenSamples) const;

    nfp_cpp *cpp_;
    uint32_t macClockXpbAddress_;
    ClockCalibration calibration_;
};
