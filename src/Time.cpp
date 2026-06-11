#include "../include/Time.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <thread>
#include <time.h>

namespace
{
constexpr int64_t kNanosecondsPerSecond = 1000000000LL;
constexpr int64_t kClockStepThresholdNs = 5000000;
constexpr int64_t kModelStepThresholdNs = 20000000;
constexpr double kMaximumDriftPpm = 1000.0;
constexpr std::size_t kStartupSampleCount = 64;
constexpr std::size_t kStartupBestSampleCount = 8;
constexpr std::size_t kRefreshBurstSize = 8;
constexpr int kRegisterReadRetries = 4;

int64_t clockNanoseconds(clockid_t clockId)
{
    timespec value{};
    if (clock_gettime(clockId, &value) != 0) {
        throw std::runtime_error("clock_gettime failed during time calibration");
    }
    return static_cast<int64_t>(value.tv_sec) * kNanosecondsPerSecond +
           static_cast<int64_t>(value.tv_nsec);
}

int64_t midpoint(int64_t first, int64_t second)
{
    return first + (second - first) / 2;
}

int64_t median(std::vector<int64_t> values)
{
    if (values.empty()) {
        return 0;
    }
    const std::size_t middle = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + middle, values.end());
    const int64_t upper = values[middle];
    if ((values.size() & 1U) != 0U) {
        return upper;
    }
    std::nth_element(values.begin(), values.begin() + middle - 1, values.end());
    return midpoint(values[middle - 1], upper);
}

bool fitLine(const std::vector<TimeSample> &samples,
             int64_t macOrigin,
             int64_t unixOrigin,
             double &scale,
             int64_t &fittedUnixOrigin)
{
    if (samples.size() < 2) {
        return false;
    }

    long double sumX = 0.0L;
    long double sumY = 0.0L;
    for (const auto &sample : samples) {
        sumX += static_cast<long double>(sample.mac_ns - macOrigin);
        sumY += static_cast<long double>(sample.unix_ns - unixOrigin);
    }
    const long double count = static_cast<long double>(samples.size());
    const long double meanX = sumX / count;
    const long double meanY = sumY / count;

    long double numerator = 0.0L;
    long double denominator = 0.0L;
    for (const auto &sample : samples) {
        const long double x =
            static_cast<long double>(sample.mac_ns - macOrigin) - meanX;
        const long double y =
            static_cast<long double>(sample.unix_ns - unixOrigin) - meanY;
        numerator += x * y;
        denominator += x * x;
    }
    if (denominator == 0.0L) {
        return false;
    }

    scale = static_cast<double>(numerator / denominator);
    if (!std::isfinite(scale)) {
        return false;
    }
    const long double originAdjustment =
        meanY - static_cast<long double>(scale) * meanX;
    fittedUnixOrigin =
        unixOrigin + static_cast<int64_t>(std::llround(originAdjustment));
    return true;
}

int64_t applyModel(int64_t macNs,
                   int64_t macOriginNs,
                   int64_t unixOriginNs,
                   double scale)
{
    const long double delta =
        static_cast<long double>(macNs - macOriginNs) *
        static_cast<long double>(scale);
    return unixOriginNs + static_cast<int64_t>(std::llround(delta));
}
} // namespace

bool composeAtomicMacTime(uint32_t secondsBefore,
                          uint32_t nanoseconds,
                          uint32_t secondsAfter,
                          int64_t &macTimeNs)
{
    if (secondsBefore != secondsAfter ||
        nanoseconds >= static_cast<uint32_t>(kNanosecondsPerSecond)) {
        return false;
    }
    macTimeNs =
        static_cast<int64_t>(secondsAfter) * kNanosecondsPerSecond +
        static_cast<int64_t>(nanoseconds);
    return true;
}

bool ClockCalibration::initialize(const std::vector<TimeSample> &samples)
{
    reset();

    std::vector<TimeSample> accepted;
    accepted.reserve(samples.size());
    for (const auto &sample : samples) {
        if (sample.latency_ns >= 0 &&
            sample.latency_ns <= kMaximumSampleLatencyNs &&
            sample.mac_ns > 0 && sample.unix_ns > 0) {
            accepted.push_back(sample);
        } else {
            ++status_.rejected_samples;
        }
    }
    if (accepted.size() < 4) {
        return false;
    }

    std::sort(accepted.begin(), accepted.end(),
              [](const TimeSample &left, const TimeSample &right) {
                  return left.latency_ns < right.latency_ns;
              });
    if (accepted.size() > kStartupBestSampleCount) {
        accepted.resize(kStartupBestSampleCount);
    }

    setOffsetModel(accepted);
    std::sort(accepted.begin(), accepted.end(),
              [](const TimeSample &left, const TimeSample &right) {
                  return left.monotonic_ns < right.monotonic_ns;
              });
    for (const auto &sample : accepted) {
        samples_.push_back(sample);
    }
    const auto &last = accepted.back();
    last_mac_ns_ = last.mac_ns;
    last_unix_ns_ = last.unix_ns;
    last_monotonic_ns_ = last.monotonic_ns;
    return true;
}

void ClockCalibration::setOffsetModel(const std::vector<TimeSample> &samples)
{
    std::vector<int64_t> offsets;
    offsets.reserve(samples.size());
    int64_t minimumLatency = std::numeric_limits<int64_t>::max();
    for (const auto &sample : samples) {
        offsets.push_back(sample.unix_ns - sample.mac_ns);
        minimumLatency = std::min(minimumLatency, sample.latency_ns);
    }

    const auto best = std::min_element(
        samples.begin(), samples.end(),
        [](const TimeSample &left, const TimeSample &right) {
            return left.latency_ns < right.latency_ns;
        });
    status_.mac_origin_ns = best->mac_ns;
    status_.unix_origin_ns = best->mac_ns + median(std::move(offsets));
    status_.minimum_latency_ns = minimumLatency;
    status_.median_residual_ns = 0;
    status_.scale = 1.0;
    status_.sample_count = samples.size();
    status_.valid = true;
    status_.drift_calibrated = false;
}

bool ClockCalibration::addSample(const TimeSample &sample)
{
    if (sample.latency_ns < 0 ||
        sample.latency_ns > kMaximumSampleLatencyNs ||
        sample.mac_ns <= 0 || sample.unix_ns <= 0) {
        ++status_.rejected_samples;
        return false;
    }

    if (!status_.valid) {
        status_.mac_origin_ns = sample.mac_ns;
        status_.unix_origin_ns = sample.unix_ns;
        status_.minimum_latency_ns = sample.latency_ns;
        status_.scale = 1.0;
        status_.sample_count = 1;
        status_.valid = true;
        samples_.push_back(sample);
    } else {
        const int64_t realtimeElapsed = sample.unix_ns - last_unix_ns_;
        const int64_t monotonicElapsed =
            sample.monotonic_ns - last_monotonic_ns_;
        const bool macRollback = sample.mac_ns <= last_mac_ns_;
        const bool hostClockStep =
            last_monotonic_ns_ != 0 &&
            std::llabs(realtimeElapsed - monotonicElapsed) >
                kClockStepThresholdNs;
        const bool modelStep =
            std::llabs(toUnixNanoseconds(sample.mac_ns) - sample.unix_ns) >
                kModelStepThresholdNs;

        if (macRollback || hostClockStep || modelStep) {
            const std::size_t rejected = status_.rejected_samples + 1;
            reset();
            status_.rejected_samples = rejected;
            status_.mac_origin_ns = sample.mac_ns;
            status_.unix_origin_ns = sample.unix_ns;
            status_.minimum_latency_ns = sample.latency_ns;
            status_.scale = 1.0;
            status_.sample_count = 1;
            status_.valid = true;
            samples_.push_back(sample);
        } else {
            samples_.push_back(sample);
            status_.minimum_latency_ns =
                std::min(status_.minimum_latency_ns, sample.latency_ns);
        }
    }

    last_mac_ns_ = sample.mac_ns;
    last_unix_ns_ = sample.unix_ns;
    last_monotonic_ns_ = sample.monotonic_ns;

    while (!samples_.empty() &&
           sample.monotonic_ns - samples_.front().monotonic_ns >
               kRegressionWindowNs) {
        samples_.pop_front();
    }
    status_.sample_count = samples_.size();

    if (samples_.size() >= 10 &&
        samples_.back().monotonic_ns - samples_.front().monotonic_ns >=
            kMinimumRegressionSpanNs) {
        fit();
    }
    return true;
}

bool ClockCalibration::fit()
{
    std::vector<TimeSample> candidates(samples_.begin(), samples_.end());
    const int64_t macOrigin = candidates.front().mac_ns;
    const int64_t unixOrigin = candidates.front().unix_ns;
    double initialScale = 1.0;
    int64_t initialUnixOrigin = unixOrigin;
    if (!fitLine(candidates, macOrigin, unixOrigin,
                 initialScale, initialUnixOrigin)) {
        return false;
    }

    std::vector<int64_t> residuals;
    residuals.reserve(candidates.size());
    for (const auto &sample : candidates) {
        residuals.push_back(
            sample.unix_ns -
            applyModel(sample.mac_ns, macOrigin,
                       initialUnixOrigin, initialScale));
    }
    const int64_t residualMedian = median(residuals);

    std::vector<int64_t> deviations;
    deviations.reserve(residuals.size());
    for (const int64_t residual : residuals) {
        deviations.push_back(std::llabs(residual - residualMedian));
    }
    const int64_t mad = median(deviations);
    const int64_t threshold = std::max<int64_t>(50000, mad * 6);

    std::vector<TimeSample> inliers;
    inliers.reserve(candidates.size());
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (std::llabs(residuals[i] - residualMedian) <= threshold) {
            inliers.push_back(candidates[i]);
        }
    }
    if (inliers.size() < 4) {
        return false;
    }

    const int64_t finalMacOrigin = inliers.front().mac_ns;
    const int64_t finalUnixOrigin = inliers.front().unix_ns;
    double finalScale = 1.0;
    int64_t fittedUnixOrigin = finalUnixOrigin;
    if (!fitLine(inliers, finalMacOrigin, finalUnixOrigin,
                 finalScale, fittedUnixOrigin)) {
        return false;
    }
    if (std::abs((finalScale - 1.0) * 1000000.0) > kMaximumDriftPpm) {
        return false;
    }

    std::vector<int64_t> finalResiduals;
    finalResiduals.reserve(inliers.size());
    for (const auto &sample : inliers) {
        finalResiduals.push_back(std::llabs(
            sample.unix_ns -
            applyModel(sample.mac_ns,
                       finalMacOrigin,
                       fittedUnixOrigin,
                       finalScale)));
    }

    status_.mac_origin_ns = finalMacOrigin;
    status_.unix_origin_ns = fittedUnixOrigin;
    status_.scale = finalScale;
    status_.median_residual_ns = median(std::move(finalResiduals));
    status_.sample_count = inliers.size();
    status_.drift_calibrated = true;
    return true;
}

int64_t ClockCalibration::toUnixNanoseconds(int64_t mac_ns) const
{
    if (!status_.valid) {
        throw std::runtime_error("SmartNIC clock calibration is not valid");
    }
    return applyModel(mac_ns,
                      status_.mac_origin_ns,
                      status_.unix_origin_ns,
                      status_.scale);
}

CalibrationStatus ClockCalibration::status() const
{
    return status_;
}

void ClockCalibration::reset()
{
    const std::size_t rejected = status_.rejected_samples;
    samples_.clear();
    status_ = {};
    status_.rejected_samples = rejected;
    last_mac_ns_ = 0;
    last_unix_ns_ = 0;
    last_monotonic_ns_ = 0;
}

TimeAdjuster::TimeAdjuster(nfp_cpp *cpp, uint32_t macClockXpbAddress)
    : cpp_(cpp), macClockXpbAddress_(macClockXpbAddress)
{
    if (!cpp_) {
        throw std::invalid_argument("TimeAdjuster requires a valid CPP handle");
    }
    const auto samples = collectSamples(kStartupSampleCount, true);
    if (!calibration_.initialize(samples)) {
        throw std::runtime_error(
            "Unable to calibrate SmartNIC clock: fewer than four "
            "samples completed within 800 microseconds");
    }
}

bool TimeAdjuster::readSample(TimeSample &sample) const
{
    const int64_t realtimeBefore = clockNanoseconds(CLOCK_REALTIME);
    const int64_t monotonicBefore = clockNanoseconds(CLOCK_MONOTONIC_RAW);

    uint32_t secondsBefore = 0;
    uint32_t secondsAfter = 0;
    uint32_t nanoseconds = 0;
    int64_t macTimeNs = 0;
    bool valid = false;
    for (int attempt = 0; attempt < kRegisterReadRetries; ++attempt) {
        if (nfp_xpb_readl(cpp_, macClockXpbAddress_ + 4,
                          &secondsBefore) < 0 ||
            nfp_xpb_readl(cpp_, macClockXpbAddress_,
                          &nanoseconds) < 0 ||
            nfp_xpb_readl(cpp_, macClockXpbAddress_ + 4,
                          &secondsAfter) < 0) {
            return false;
        }
        if (composeAtomicMacTime(secondsBefore, nanoseconds,
                                 secondsAfter, macTimeNs)) {
            valid = true;
            break;
        }
    }

    const int64_t monotonicAfter = clockNanoseconds(CLOCK_MONOTONIC_RAW);
    const int64_t realtimeAfter = clockNanoseconds(CLOCK_REALTIME);
    if (!valid || monotonicAfter < monotonicBefore) {
        return false;
    }

    sample.mac_ns = macTimeNs;
    sample.unix_ns = midpoint(realtimeBefore, realtimeAfter);
    sample.monotonic_ns = midpoint(monotonicBefore, monotonicAfter);
    sample.latency_ns = monotonicAfter - monotonicBefore;
    return true;
}

std::vector<TimeSample> TimeAdjuster::collectSamples(
    std::size_t count,
    bool pauseBetweenSamples) const
{
    std::vector<TimeSample> samples;
    samples.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        TimeSample sample;
        if (readSample(sample)) {
            samples.push_back(sample);
        }
        if (pauseBetweenSamples && i + 1 < count) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    return samples;
}

bool TimeAdjuster::refresh()
{
    auto samples = collectSamples(kRefreshBurstSize, false);
    if (samples.empty()) {
        return false;
    }
    const auto best = std::min_element(
        samples.begin(), samples.end(),
        [](const TimeSample &left, const TimeSample &right) {
            return left.latency_ns < right.latency_ns;
        });
    return calibration_.addSample(*best);
}

int64_t TimeAdjuster::toUnixNanoseconds(uint32_t sec, uint32_t nsec) const
{
    if (nsec >= static_cast<uint32_t>(kNanosecondsPerSecond)) {
        throw std::invalid_argument("SmartNIC timestamp nanoseconds are invalid");
    }
    const int64_t macNs =
        static_cast<int64_t>(sec) * kNanosecondsPerSecond +
        static_cast<int64_t>(nsec);
    return calibration_.toUnixNanoseconds(macNs);
}

CalibrationStatus TimeAdjuster::status() const
{
    return calibration_.status();
}
