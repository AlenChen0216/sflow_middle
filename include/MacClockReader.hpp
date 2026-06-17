#pragma once

#include "ClockSampler.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

extern "C" {
#include <nfp.h>
#include <nfp_cpp.h>
#include <nfp_nffw.h>
}

struct MacClockReaderStatus
{
    std::map<std::string, std::size_t> reject_reasons;
    std::size_t read_attempts = 0;
    std::size_t read_failures = 0;
    std::size_t valid_reads = 0;
};

class MacClockReader : public IClockStateReader
{
public:
    MacClockReader(nfp_cpp *cpp,
                   const nfp_rtsym *symbol,
                   std::string symbolName = "mac_time",
                   std::size_t expectedSize = sizeof(MacTimeStateHost));

    MacStateObservation readState() override;
    MacClockReaderStatus status() const;

    const std::string &symbolName() const noexcept { return symbolName_; }
    uint64_t symbolAddress() const noexcept { return address_; }
    int symbolDomain() const noexcept { return domain_; }
    int symbolTarget() const noexcept { return target_; }
    uint32_t cppId() const noexcept { return cppId_; }

private:
    bool readRaw(MacTimeStateHost &state, std::string &reason);
    bool validateState(const MacTimeStateHost &state, std::string &reason);
    void reject(const std::string &reason);

    nfp_cpp *cpp_;
    std::string symbolName_;
    uint64_t address_;
    uint64_t symbolSize_;
    int domain_;
    int target_;
    int type_;
    std::size_t expectedSize_;
    uint32_t cppId_;
    bool hasLastState_ = false;
    MacTimeStateHost lastState_;
    MacClockReaderStatus status_;
};

