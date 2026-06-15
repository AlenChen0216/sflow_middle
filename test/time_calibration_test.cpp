#include "../include/Time.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
constexpr int64_t kSecond = 1000000000LL;
constexpr int64_t kMacBase = 2000000000000LL;
constexpr int64_t kUnixBase = 1780000000000000000LL;

void require(bool condition, const std::string &message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

TimeSample sampleAt(int64_t elapsedNs,
                    double scale = 1.0,
                    int64_t noiseNs = 0,
                    int64_t latencyNs = 100000)
{
    return {
        kMacBase + elapsedNs,
        kUnixBase +
            static_cast<int64_t>(std::llround(
                static_cast<long double>(elapsedNs) * scale)) +
            noiseNs,
        500000000000LL + elapsedNs,
        latencyNs,
    };
}

std::vector<TimeSample> startupSamples()
{
    std::vector<TimeSample> samples;
    for (int i = 0; i < 16; ++i) {
        samples.push_back(sampleAt(
            static_cast<int64_t>(i) * 1000000,
            1.0,
            (i % 3 - 1) * 10000,
            50000 + i * 10000));
    }
    return samples;
}

void testAtomicMacTime()
{
    int64_t macNs = 0;
    require(composeAtomicMacTime(12, 345, 12, macNs),
            "stable register read should be accepted");
    require(macNs == 12 * kSecond + 345,
            "stable register read produced the wrong timestamp");
    require(!composeAtomicMacTime(12, 999999999, 13, macNs),
            "seconds rollover must force a retry");
    require(!composeAtomicMacTime(12, 1000000000, 12, macNs),
            "invalid nanoseconds must be rejected");
}

void testStartupOffsetAndLatencyFiltering()
{
    auto samples = startupSamples();
    samples.push_back(sampleAt(20000000, 1.0, 50000000, 900000));

    ClockCalibration calibration;
    require(calibration.initialize(samples), "startup calibration failed");
    const auto status = calibration.status();
    require(status.valid, "startup model is not valid");
    require(status.sample_count == 8,
            "startup must retain the eight lowest-latency samples");
    require(status.minimum_latency_ns == 50000,
            "minimum sample latency was not recorded");
    require(status.rejected_samples == 1,
            "high-latency startup sample was not rejected");

    const int64_t converted =
        calibration.toUnixNanoseconds(kMacBase + 500000000);
    require(std::llabs(converted - (kUnixBase + 500000000)) < 20000,
            "startup offset model is inaccurate");
}

void testRobustDriftFit()
{
    ClockCalibration calibration;
    require(calibration.initialize(startupSamples()),
            "drift test startup calibration failed");

    constexpr double scale = 1.000020;
    for (int second = 1; second <= 30; ++second) {
        int64_t noise = (second % 5 - 2) * 15000;
        if (second == 17) {
            noise += 2000000;
        }
        require(calibration.addSample(
                    sampleAt(second * kSecond, scale, noise, 80000)),
                "valid drift sample was rejected");
    }

    const auto status = calibration.status();
    require(status.drift_calibrated, "drift model was not fitted");
    require(std::abs(status.scale - scale) < 0.000002,
            "fitted drift differs by more than two ppm");
    require(status.median_residual_ns < 100000,
            "robust fit residual is too large");

    std::vector<int64_t> errors;
    for (int i = 0; i < 1000; ++i) {
        const int64_t elapsed = 31 * kSecond + i * 1000000LL;
        const int64_t expected =
            kUnixBase + static_cast<int64_t>(std::llround(
                static_cast<long double>(elapsed) * scale));
        errors.push_back(std::llabs(
            calibration.toUnixNanoseconds(kMacBase + elapsed) - expected));
    }
    std::sort(errors.begin(), errors.end());
    require(errors[989] < 500000,
            "synthetic P99 mapping error exceeds 500 microseconds");
}

void testDriftFitDuringShortRun()
{
    ClockCalibration calibration;
    require(calibration.initialize(startupSamples()),
            "short-run drift test startup calibration failed");

    constexpr double scale = 1.000500;
    for (int second = 1; second <= 3; ++second) {
        require(calibration.addSample(
                    sampleAt(second * kSecond, scale, 0, 80000)),
                "short-run drift sample was rejected");
    }

    const auto status = calibration.status();
    require(status.drift_calibrated,
            "drift calibration did not become active during a short run");
    require(std::abs(status.scale - scale) < 0.000010,
            "short-run drift estimate differs by more than ten ppm");

    const int64_t elapsed = 4 * kSecond;
    const int64_t expected =
        kUnixBase + static_cast<int64_t>(std::llround(
            static_cast<long double>(elapsed) * scale));
    require(std::llabs(
                calibration.toUnixNanoseconds(kMacBase + elapsed) -
                expected) < 100000,
            "short-run drift compensation exceeds 100 microseconds");
}

void testClockResets()
{
    ClockCalibration calibration;
    require(calibration.initialize(startupSamples()),
            "reset test startup calibration failed");

    require(calibration.addSample(sampleAt(kSecond)),
            "normal sample was rejected");
    TimeSample rollback = sampleAt(2 * kSecond);
    rollback.mac_ns = kMacBase - 1;
    require(calibration.addSample(rollback),
            "MAC rollback sample should establish a fresh model");
    auto status = calibration.status();
    require(status.valid && status.sample_count == 1 &&
                !status.drift_calibrated,
            "MAC rollback did not reset calibration");

    TimeSample hostStep = sampleAt(3 * kSecond);
    hostStep.unix_ns += 10000000;
    require(calibration.addSample(hostStep),
            "host clock step sample should establish a fresh model");
    status = calibration.status();
    require(status.sample_count == 1 && !status.drift_calibrated,
            "host clock step did not reset calibration");
}

void testLargeEpochAndMonotonicConversion()
{
    ClockCalibration calibration;
    require(calibration.initialize(startupSamples()),
            "large epoch startup calibration failed");
    const int64_t first =
        calibration.toUnixNanoseconds(kMacBase + 100);
    const int64_t second =
        calibration.toUnixNanoseconds(kMacBase + 101);
    require(first > 1700000000000000000LL,
            "large Unix epoch lost precision");
    require(second > first, "increasing MAC time must increase Unix time");
}
} // namespace

int main()
{
    try {
        testAtomicMacTime();
        testStartupOffsetAndLatencyFiltering();
        testRobustDriftFit();
        testDriftFitDuringShortRun();
        testClockResets();
        testLargeEpochAndMonotonicConversion();
    } catch (const std::exception &ex) {
        std::cerr << "time calibration test failed: " << ex.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "time calibration tests passed\n";
    return EXIT_SUCCESS;
}
