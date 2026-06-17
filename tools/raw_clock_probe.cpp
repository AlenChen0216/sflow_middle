#include "../include/ClockSampler.hpp"
#include "../include/Device.hpp"
#include "../include/MacClockReader.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <time.h>

namespace
{
int64_t clockNanoseconds(clockid_t clockId)
{
    timespec value{};
    if (clock_gettime(clockId, &value) != 0) {
        throw std::runtime_error("clock_gettime failed");
    }
    return static_cast<int64_t>(value.tv_sec) * kNanosecondsPerSecond +
           static_cast<int64_t>(value.tv_nsec);
}
} // namespace

int main(int argc, char *argv[])
{
    const unsigned int devnum = argc > 1
        ? static_cast<unsigned int>(std::stoul(argv[1]))
        : 0;
    const std::string symbolName = argc > 2 ? argv[2] : "_mac_time";
    const int samples = argc > 3 ? std::stoi(argv[3]) : 1000;
    const auto interval = std::chrono::microseconds(
        argc > 4 ? std::stoi(argv[4]) : 1000);

    try {
        NFPDevice device(devnum);
        const nfp_rtsym *symbol =
            device.getSymbolData(symbolName.c_str());
        if (!symbol) {
            std::cerr << "Symbol not found: " << symbolName << "\n";
            return EXIT_FAILURE;
        }

        MacClockReader reader(device.cpp(), symbol, symbolName);
        bool havePrevious = false;
        MacTimeStateHost previous;

        std::cout
            << "realtime_before_ns,realtime_after_ns,"
            << "monotonic_before_ns,monotonic_after_ns,"
            << "read_latency_ns,valid,reject_reason,"
            << "mac_time_s,mac_time_ns,me_time,conv_mult,conv_rshift,"
            << "state_changed\n";

        for (int i = 0; i < samples; ++i) {
            HostClockBounds bounds;
            bounds.realtime_before_ns = clockNanoseconds(CLOCK_REALTIME);
            bounds.monotonic_before_ns = clockNanoseconds(CLOCK_MONOTONIC_RAW);
            MacStateObservation observation = reader.readState();
            bounds.monotonic_after_ns = clockNanoseconds(CLOCK_MONOTONIC_RAW);
            bounds.realtime_after_ns = clockNanoseconds(CLOCK_REALTIME);
            observation.bounds = bounds;
            observation.read_latency_ns =
                bounds.monotonic_after_ns - bounds.monotonic_before_ns;

            const bool changed =
                observation.valid &&
                (!havePrevious || observation.state != previous);
            if (observation.valid) {
                previous = observation.state;
                havePrevious = true;
            }

            std::cout
                << bounds.realtime_before_ns << ','
                << bounds.realtime_after_ns << ','
                << bounds.monotonic_before_ns << ','
                << bounds.monotonic_after_ns << ','
                << observation.read_latency_ns << ','
                << (observation.valid ? 1 : 0) << ','
                << observation.reject_reason << ','
                << observation.state.mac_time_s << ','
                << observation.state.mac_time_ns << ','
                << observation.state.me_time << ','
                << observation.state.conv_mult << ','
                << observation.state.conv_rshift << ','
                << (changed ? 1 : 0) << '\n';

            std::this_thread::sleep_for(interval);
        }
    } catch (const std::exception &ex) {
        std::cerr << "raw_clock_probe failed: " << ex.what() << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

