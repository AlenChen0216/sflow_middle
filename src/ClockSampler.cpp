#include "../include/ClockSampler.hpp"

#include <cmath>
#include <cstdlib>

bool MacTimeStateHost::operator==(const MacTimeStateHost &other) const
{
    return mac_time_s == other.mac_time_s &&
           mac_time_ns == other.mac_time_ns &&
           me_time == other.me_time &&
           conv_mult == other.conv_mult &&
           conv_rshift == other.conv_rshift;
}

bool MacTimeStateHost::operator!=(const MacTimeStateHost &other) const
{
    return !(*this == other);
}

ClockSampler::ClockSampler(IClockStateReader &reader,
                           ClockFn realtimeClock,
                           ClockFn monotonicClock,
                           int64_t maximumUncertaintyNs)
    : reader_(reader),
      realtimeClock_(std::move(realtimeClock)),
      monotonicClock_(std::move(monotonicClock)),
      maximumUncertaintyNs_(maximumUncertaintyNs)
{
}

std::optional<PublicationSample> ClockSampler::poll()
{
    ++counters_.observations;

    HostClockBounds bounds;
    bounds.realtime_before_ns = realtimeClock_();
    bounds.monotonic_before_ns = monotonicClock_();
    MacStateObservation observation = reader_.readState();
    bounds.monotonic_after_ns = monotonicClock_();
    bounds.realtime_after_ns = realtimeClock_();
    observation.bounds = bounds;
    observation.read_latency_ns =
        bounds.monotonic_after_ns - bounds.monotonic_before_ns;

    if (bounds.realtime_after_ns < bounds.realtime_before_ns ||
        bounds.monotonic_after_ns < bounds.monotonic_before_ns) {
        ++counters_.invalid_observations;
        observation.valid = false;
        observation.reject_reason = "host-clock-rollback";
        return std::nullopt;
    }

    const int64_t realtimeMidpoint =
        midpoint(bounds.realtime_before_ns, bounds.realtime_after_ns);
    const int64_t monotonicMidpoint =
        midpoint(bounds.monotonic_before_ns, bounds.monotonic_after_ns);
    if (previousMonotonicMidpointNs_ != 0) {
        const int64_t realtimeDelta =
            realtimeMidpoint - previousRealtimeMidpointNs_;
        const int64_t monotonicDelta =
            monotonicMidpoint - previousMonotonicMidpointNs_;
        if (std::llabs(realtimeDelta - monotonicDelta) > 500000) {
            ++counters_.host_clock_steps;
            reset();
        }
    }
    previousRealtimeMidpointNs_ = realtimeMidpoint;
    previousMonotonicMidpointNs_ = monotonicMidpoint;

    if (!observation.valid) {
        ++counters_.invalid_observations;
        return std::nullopt;
    }

    if (!lastObservation_) {
        lastObservation_ = observation;
        return std::nullopt;
    }

    if (observation.state == lastObservation_->state) {
        ++counters_.duplicate_states;
        lastObservation_ = observation;
        return std::nullopt;
    }

    std::string reason;
    if (!isPlausibleTransition(lastObservation_->state,
                               observation.state,
                               reason)) {
        ++counters_.device_resets;
        ++counters_.rejected_transitions;
        lastObservation_ = observation;
        return std::nullopt;
    }

    int64_t smartnicNs = 0;
    if (!stateTimeNs(observation.state, smartnicNs)) {
        ++counters_.rejected_transitions;
        lastObservation_ = observation;
        return std::nullopt;
    }

    const int64_t transitionRealtimeStart =
        lastObservation_->bounds.realtime_after_ns;
    const int64_t transitionRealtimeEnd = observation.bounds.realtime_after_ns;
    const int64_t transitionMonoStart =
        lastObservation_->bounds.monotonic_after_ns;
    const int64_t transitionMonoEnd = observation.bounds.monotonic_after_ns;
    const int64_t transitionWidth = transitionMonoEnd - transitionMonoStart;
    const int64_t uncertainty = transitionWidth + observation.read_latency_ns;

    if (transitionWidth < 0 || uncertainty > maximumUncertaintyNs_) {
        ++counters_.rejected_transitions;
        lastObservation_ = observation;
        return std::nullopt;
    }

    PublicationSample sample;
    sample.smartnic_ns = smartnicNs;
    sample.host_realtime_ns =
        midpoint(transitionRealtimeStart, transitionRealtimeEnd);
    sample.host_monotonic_ns =
        midpoint(transitionMonoStart, transitionMonoEnd);
    sample.uncertainty_ns = uncertainty;
    sample.transition_start_mono_ns = transitionMonoStart;
    sample.transition_end_mono_ns = transitionMonoEnd;
    sample.me_time = observation.state.me_time;
    sample.conv_mult = observation.state.conv_mult;
    sample.conv_rshift = observation.state.conv_rshift;

    ++counters_.accepted_transitions;
    lastObservation_ = observation;
    return sample;
}

ClockSamplerCounters ClockSampler::counters() const
{
    return counters_;
}

void ClockSampler::reset()
{
    lastObservation_.reset();
    previousRealtimeMidpointNs_ = 0;
    previousMonotonicMidpointNs_ = 0;
}

bool ClockSampler::stateTimeNs(const MacTimeStateHost &state, int64_t &timeNs)
{
    if (state.mac_time_ns >= static_cast<uint32_t>(kNanosecondsPerSecond)) {
        return false;
    }
    timeNs = static_cast<int64_t>(state.mac_time_s) * kNanosecondsPerSecond +
             static_cast<int64_t>(state.mac_time_ns);
    return true;
}

int64_t ClockSampler::midpoint(int64_t first, int64_t second)
{
    return first + (second - first) / 2;
}

bool ClockSampler::isPlausibleTransition(const MacTimeStateHost &previous,
                                         const MacTimeStateHost &current,
                                         std::string &reason) const
{
    if (current.conv_mult != previous.conv_mult ||
        current.conv_rshift != previous.conv_rshift) {
        reason = "conversion-changed";
        return false;
    }

    int64_t previousNs = 0;
    int64_t currentNs = 0;
    if (!stateTimeNs(previous, previousNs) ||
        !stateTimeNs(current, currentNs)) {
        reason = "invalid-time";
        return false;
    }
    if (currentNs <= previousNs) {
        reason = "mac-time-rollback";
        return false;
    }

    return true;
}

