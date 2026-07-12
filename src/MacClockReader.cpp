#include "../include/MacClockReader.hpp"

#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <vector>

#include <spdlog/spdlog.h>

namespace
{
constexpr uint32_t kCppTargetMem = 7;
constexpr uint32_t kCppActionReadWrite = 32;

uint16_t readLe16(const std::vector<uint8_t> &bytes, std::size_t offset)
{
    uint16_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

uint32_t readLe32(const std::vector<uint8_t> &bytes, std::size_t offset)
{
    uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

bool hasValidConversion(const MacTimeStateHost &state)
{
    return state.conv_mult != 0 && state.conv_rshift < 32;
}
}

static_assert(sizeof(MacTimeStateHost) == 20,
              "MacTimeStateHost wire size changed");
static_assert(offsetof(MacTimeStateHost, mac_time_s) == 0,
              "mac_time_s offset changed");
static_assert(offsetof(MacTimeStateHost, mac_time_ns) == 4,
              "mac_time_ns offset changed");
static_assert(offsetof(MacTimeStateHost, me_time) == 8,
              "me_time offset changed");
static_assert(offsetof(MacTimeStateHost, conv_mult) == 12,
              "conv_mult offset changed");
static_assert(offsetof(MacTimeStateHost, conv_rshift) == 16,
              "conv_rshift offset changed");

MacClockReader::MacClockReader(nfp_cpp *cpp,
                               const nfp_rtsym *symbol,
                               std::string symbolName,
                               std::size_t expectedSize)
    : cpp_(cpp),
      symbolName_(std::move(symbolName)),
      address_(symbol ? symbol->addr : 0),
      symbolSize_(symbol ? symbol->size : 0),
      domain_(symbol ? symbol->domain : 0),
      target_(symbol ? symbol->target : 0),
      type_(symbol ? symbol->type : 0),
      expectedSize_(expectedSize),
      cppId_(NFP_CPP_ISLAND_ID(kCppTargetMem, kCppActionReadWrite, 0, domain_))
{
    if (!cpp_) {
        throw std::invalid_argument("MacClockReader requires a valid CPP handle");
    }
    if (!symbol) {
        throw std::invalid_argument("MacClockReader requires a resolved mac_time symbol");
    }
    if (expectedSize_ != sizeof(MacTimeStateHost)) {
        throw std::invalid_argument("MacClockReader expected size does not match host structure");
    }
    if (symbolSize_ != 0 && symbolSize_ < 16) {
        throw std::runtime_error("mac_time symbol is too small to contain mandatory fields");
    }

    SPDLOG_INFO("SmartNIC exported clock symbol {}: addr=0x{:x}, domain={}, "
                "target={}, type={}, size={}, host_size={}",
                symbolName_, address_, domain_, target_, type_, symbolSize_,
                expectedSize_);
}

MacStateObservation MacClockReader::readState()
{
    ++status_.read_attempts;

    MacStateObservation observation;
    std::string reason;
    if (!readRaw(observation.state, reason)) {
        ++status_.read_failures;
        reject(reason);
        observation.valid = false;
        observation.reject_reason = reason;
        return observation;
    }
    if (!validateState(observation.state, reason)) {
        reject(reason);
        observation.valid = false;
        observation.reject_reason = reason;
        return observation;
    }

    observation.valid = true;
    ++status_.valid_reads;
    lastState_ = observation.state;
    hasLastState_ = true;
    return observation;
}

MacClockReaderStatus MacClockReader::status() const
{
    return status_;
}

bool MacClockReader::readRaw(MacTimeStateHost &state, std::string &reason)
{
    const std::size_t bytesToRead =
        symbolSize_ >= 20 ? 20 :
        symbolSize_ >= 16 ? 16 :
        expectedSize_;
    nfp_cpp_area *area =
        nfp_cpp_area_alloc(cpp_, cppId_, address_, bytesToRead);
    if (!area) {
        reason = "area-alloc-failed";
        return false;
    }
    if (nfp_cpp_area_acquire(area) < 0) {
        nfp_cpp_area_free(area);
        reason = "area-acquire-failed";
        return false;
    }

    std::vector<uint8_t> raw(bytesToRead, 0);
    const int bytesRead =
        nfp_cpp_area_read(area, 0, raw.data(), bytesToRead);
    nfp_cpp_area_release_free(area);

    if (bytesRead != static_cast<int>(bytesToRead)) {
        reason = "short-read";
        return false;
    }

    MacTimeStateHost packed;
    packed.mac_time_s = readLe32(raw, 0);
    packed.mac_time_ns = readLe32(raw, 4);
    packed.me_time = readLe32(raw, 8);
    packed.conv_mult = readLe16(raw, 12);
    packed.conv_rshift = readLe16(raw, 14);
    if (hasValidConversion(packed)) {
        state = packed;
        return true;
    }

    MacTimeStateHost swapped = packed;
    swapped.conv_mult = readLe16(raw, 14);
    swapped.conv_rshift = readLe16(raw, 12);
    if (hasValidConversion(swapped)) {
        state = swapped;
        return true;
    }

    if (bytesToRead >= 20) {
        MacTimeStateHost aligned = packed;
        aligned.conv_mult = readLe32(raw, 12);
        aligned.conv_rshift = readLe32(raw, 16);
        if (hasValidConversion(aligned)) {
            state = aligned;
            return true;
        }
    }

    state = packed;
    return true;
}

bool MacClockReader::validateState(const MacTimeStateHost &state,
                                   std::string &reason)
{
    if (state.mac_time_ns >=
        static_cast<uint32_t>(kNanosecondsPerSecond)) {
        reason = "invalid-nanoseconds";
        return false;
    }
    if (state.conv_rshift >= 32) {
        reason = "invalid-rshift";
        return false;
    }
    if (state.conv_mult == 0) {
        reason = "invalid-conv-mult";
        return false;
    }
    if (state.mac_time_s == 0 && state.mac_time_ns == 0 &&
        state.me_time == 0 && state.conv_mult == 0 &&
        state.conv_rshift == 0) {
        reason = "all-zero-state";
        return false;
    }

    if (hasLastState_) {
        const int64_t lastNs =
            static_cast<int64_t>(lastState_.mac_time_s) *
                kNanosecondsPerSecond +
            static_cast<int64_t>(lastState_.mac_time_ns);
        const int64_t currentNs =
            static_cast<int64_t>(state.mac_time_s) *
                kNanosecondsPerSecond +
            static_cast<int64_t>(state.mac_time_ns);
        if (currentNs < lastNs) {
            reason = "mac-time-rollback";
            return false;
        }
    }

    return true;
}

void MacClockReader::reject(const std::string &reason)
{
    ++status_.reject_reasons[reason.empty() ? "unknown" : reason];
}
