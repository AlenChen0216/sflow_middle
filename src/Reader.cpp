#include "../include/Reader.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <limits>
#include <vector>
#include <iostream>

namespace {
constexpr uint32_t kCppTargetMem = 7;
constexpr uint32_t kCppActionRead = 0;
constexpr uint32_t kCppActionWrite = 32;
// constexpr uint32_t kEmemIsland = 24;
constexpr uint32_t kResetFillValue = 0;
}  // namespace




SmartNicReader::SmartNicReader(nfp_cpp *cpp, uint64_t ememBase)
    : cpp_(cpp), ememBase_(ememBase) {}

std::vector<uint8_t> SmartNicReader::readAndReset(const RegisterSpec &reg) noexcept {
    if (!cpp_ || reg.bytes == 0 || reg.buffer_size == 0) {
        return {};
    }

    const uint64_t totalBytes64 =
        static_cast<uint64_t>(reg.bytes) * static_cast<uint64_t>(reg.buffer_size);
    if (totalBytes64 == 0 || totalBytes64 > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return {};
    }
    const unsigned long totalBytes = static_cast<unsigned long>(totalBytes64);

    const uint64_t address = ememBase_ + reg.offset;

    const uint32_t readCppId =
        NFP_CPP_ISLAND_ID(kCppTargetMem, kCppActionWrite, 0, reg.reg_island);
    nfp_cpp_area *readArea = nfp_cpp_area_alloc(cpp_, readCppId, address, totalBytes);
    if (!readArea) {
        return {};
    }

    if (nfp_cpp_area_acquire(readArea) < 0) {
        nfp_cpp_area_release_free(readArea);
        return {};
    }

    std::vector<uint8_t> buffer(totalBytes, 0);

    const int bytesRead = nfp_cpp_area_read(readArea, 0, buffer.data(), totalBytes);
    if (bytesRead != static_cast<int>(totalBytes)) {
        nfp_cpp_area_release_free(readArea);
        return {};
    }
    // std::cout<< "Read " << bytesRead << " bytes from register " << reg.name << "\n";
    const int fillResult = nfp_cpp_area_fill(readArea, 0, kResetFillValue, totalBytes);
    nfp_cpp_area_release_free(readArea);
    // if (fillResult < 0) {
    //     return {};
    // }

    return buffer;
}
