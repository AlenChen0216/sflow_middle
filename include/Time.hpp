#pragma once

#include "ClockSampler.hpp"
#include "MacClockReader.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include <nfp.h>
#include <nfp_cpp.h>
#include <nfp_nffw.h>
}

enum class CalibrationState
{
    WarmingUp,
    OffsetOnly,
    Tracking,
    Degraded,
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
    std::size_t accepted_samples = 0;
    std::size_t duplicate_states = 0;
    std::size_t invalid_observations = 0;
    std::size_t rejected_transitions = 0;
    std::size_t host_clock_steps = 0;
    std::size_t device_resets = 0;
    int64_t latest_uncertainty_ns = 0;
    int64_t sample_span_ns = 0;
    CalibrationState state = CalibrationState::WarmingUp;
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

    struct Config
    {
        int64_t maximum_sample_latency_ns = kMaximumSampleLatencyNs;
        int64_t regression_window_ns = kRegressionWindowNs;
        int64_t minimum_regression_span_ns = kMinimumRegressionSpanNs;
        int64_t host_step_threshold_ns = 500000;
        int64_t model_step_threshold_ns = 20000000;
        double maximum_drift_ppm = 1000.0;
    };

    ClockCalibration();
    explicit ClockCalibration(Config config);

    bool initialize(const std::vector<TimeSample> &samples);
    bool addSample(const TimeSample &sample);
    int64_t toUnixNanoseconds(int64_t mac_ns) const;
    CalibrationStatus status() const;
    void reset();

private:
    bool fit();
    void setOffsetModel(const std::vector<TimeSample> &samples);

    Config config_;
    std::deque<TimeSample> samples_;
    CalibrationStatus status_;
    int64_t last_mac_ns_ = 0;
    int64_t last_unix_ns_ = 0;
    int64_t last_monotonic_ns_ = 0;
};

class TimeAdjuster
{
public:
    struct Config
    {
        std::string mac_time_symbol = "mac_time";
        std::size_t startup_sample_count = 64;
        std::size_t startup_best_sample_count = 8;
        std::size_t refresh_burst_size = 8;
        int64_t maximum_sample_uncertainty_ns = 800000;
        ClockCalibration::Config calibration;
        std::chrono::milliseconds startup_timeout{2000};
        std::chrono::microseconds startup_poll_interval{1000};
    };

    TimeAdjuster(nfp_cpp *cpp,
                 const nfp_rtsym *macTimeSymbol,
                 Config config);

    bool refresh();
    int64_t toUnixNanoseconds(uint32_t sec, uint32_t nsec) const;
    CalibrationStatus status() const;

private:
    std::vector<PublicationSample> collectPublicationSamples(
        std::size_t count,
        std::chrono::steady_clock::time_point deadline,
        bool pauseBetweenPolls);
    static TimeSample toTimeSample(const PublicationSample &sample);
    bool initializeFromPublicationSamples(
        const std::vector<PublicationSample> &samples);
    void mergeSamplerStatus();

    nfp_cpp *cpp_;
    Config config_;
    std::unique_ptr<MacClockReader> reader_;
    std::unique_ptr<ClockSampler> sampler_;
    ClockCalibration calibration_;
    CalibrationStatus status_cache_;
};
