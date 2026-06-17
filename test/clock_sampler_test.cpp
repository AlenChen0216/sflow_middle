#include "../include/ClockSampler.hpp"

#include <cstdlib>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
void require(bool condition, const std::string &message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class FakeReader : public IClockStateReader
{
public:
    explicit FakeReader(std::deque<MacTimeStateHost> states)
        : states_(std::move(states))
    {
    }

    MacStateObservation readState() override
    {
        MacStateObservation observation;
        if (states_.empty()) {
            observation.valid = false;
            observation.reject_reason = "empty";
            return observation;
        }
        observation.state = states_.front();
        states_.pop_front();
        observation.valid = true;
        return observation;
    }

private:
    std::deque<MacTimeStateHost> states_;
};

class FakeClock
{
public:
    explicit FakeClock(std::deque<int64_t> values)
        : values_(std::move(values))
    {
    }

    int64_t operator()()
    {
        if (values_.empty()) {
            throw std::runtime_error("fake clock exhausted");
        }
        const int64_t value = values_.front();
        values_.pop_front();
        return value;
    }

private:
    std::deque<int64_t> values_;
};

MacTimeStateHost state(uint32_t seconds, uint32_t nanoseconds)
{
    MacTimeStateHost value;
    value.mac_time_s = seconds;
    value.mac_time_ns = nanoseconds;
    value.me_time = seconds * 1000 + nanoseconds / 1000000;
    value.conv_mult = 16;
    value.conv_rshift = 0;
    return value;
}

void testDuplicateStatesDoNotEmitSamples()
{
    FakeReader reader({state(10, 0), state(10, 0), state(10, 0)});
    FakeClock realtime({1000, 1100, 2000, 2100, 3000, 3100});
    FakeClock monotonic({500, 600, 1500, 1600, 2500, 2600});
    ClockSampler sampler(
        reader,
        [&] { return realtime(); },
        [&] { return monotonic(); },
        10000);

    require(!sampler.poll(), "first observation must seed the detector");
    require(!sampler.poll(), "duplicate state must not emit a sample");
    require(!sampler.poll(), "duplicate state must still not emit a sample");

    const auto counters = sampler.counters();
    require(counters.duplicate_states == 2,
            "duplicate states were not counted");
    require(counters.accepted_transitions == 0,
            "duplicate states emitted transition samples");
}

void testTransitionIntervalSample()
{
    FakeReader reader({state(10, 0), state(10, 0), state(10, 1000)});
    FakeClock realtime({1000, 1100, 2000, 2100, 3000, 3100});
    FakeClock monotonic({500, 600, 1500, 1600, 2500, 2600});
    ClockSampler sampler(
        reader,
        [&] { return realtime(); },
        [&] { return monotonic(); },
        10000);

    require(!sampler.poll(), "first observation must not emit");
    require(!sampler.poll(), "duplicate observation must not emit");
    const auto sample = sampler.poll();
    require(sample.has_value(), "changed state should emit a transition");
    require(sample->smartnic_ns == 10 * kNanosecondsPerSecond + 1000,
            "sample contains the wrong SmartNIC timestamp");
    require(sample->host_realtime_ns == 2600,
            "sample must use the transition realtime midpoint");
    require(sample->host_monotonic_ns == 2100,
            "sample must use the transition monotonic midpoint");
    require(sample->uncertainty_ns == 1100,
            "sample uncertainty should include transition width and read latency");
}

void testRollbackIsRejectedAsReset()
{
    FakeReader reader({state(10, 1000), state(9, 999999999)});
    FakeClock realtime({1000, 1100, 2000, 2100});
    FakeClock monotonic({500, 600, 1500, 1600});
    ClockSampler sampler(
        reader,
        [&] { return realtime(); },
        [&] { return monotonic(); },
        10000);

    require(!sampler.poll(), "first observation must seed the detector");
    require(!sampler.poll(), "rollback must be rejected");
    const auto counters = sampler.counters();
    require(counters.device_resets == 1, "rollback was not counted as reset");
    require(counters.rejected_transitions == 1,
            "rollback was not counted as a rejected transition");
}
} // namespace

int main()
{
    try {
        testDuplicateStatesDoNotEmitSamples();
        testTransitionIntervalSample();
        testRollbackIsRejectedAsReset();
    } catch (const std::exception &ex) {
        std::cerr << "clock sampler test failed: " << ex.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "clock sampler tests passed\n";
    return EXIT_SUCCESS;
}

