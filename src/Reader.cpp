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
constexpr uint32_t kSemLockValue = 4;
constexpr uint32_t kSemUnlockValue = 0;
constexpr uint32_t kDataClearValue = 0;
constexpr uint32_t kSemDataReady = 3;
}  // namespace

SmartNicReader::SmartNicReader(nfp_cpp *cpp,
                               uint64_t semAddr0, uint64_t semAddr1,
                               uint64_t dataAddr0, uint64_t dataAddr1,
                               uint32_t semIsland, uint32_t dataIsland,
                               size_t slotCount)
    : cpp_(cpp), slotCount_(slotCount), bufferState_(0)
{
    semAddr_[0] = semAddr0;
    semAddr_[1] = semAddr1;
    dataAddr_[0] = dataAddr0;
    dataAddr_[1] = dataAddr1;

    semSize_ = static_cast<unsigned long>(slotCount_ * sizeof(uint32_t));
    dataSize_ = static_cast<unsigned long>(slotCount_ * sizeof(flow_data));

    semCppId_ = NFP_CPP_ISLAND_ID(kCppTargetMem, kCppActionWrite, 0, semIsland);
    dataCppId_ = NFP_CPP_ISLAND_ID(kCppTargetMem, kCppActionWrite, 0, dataIsland);
}

std::vector<flow_data> SmartNicReader::readFlowData() noexcept {
    if (!cpp_ || slotCount_ == 0) {
        return {};
    }

    std::vector<flow_data> result;

    // -------------------------------------------------------------------------
    // Step 1: Read the _global_semaphores of the active buffer
    // -------------------------------------------------------------------------
    const uint64_t semAddr = semAddr_[bufferState_];
    nfp_cpp_area *semArea = nfp_cpp_area_alloc(cpp_, semCppId_, semAddr, semSize_);
    if (!semArea) {
        return {};
    }
    if (nfp_cpp_area_acquire(semArea) < 0) {
        nfp_cpp_area_free(semArea);
        return {};
    }

    std::vector<uint32_t> semaphores(slotCount_, 0);
    int bytesRead = nfp_cpp_area_read(semArea, 0, semaphores.data(), semSize_);
    if (bytesRead != static_cast<int>(semSize_)) {
        nfp_cpp_area_release_free(semArea);
        return {};
    }

    // -------------------------------------------------------------------------
    // Step 2: Fill the _global_semaphores to 4 (lock — tell SmartNIC we are reading)
    // -------------------------------------------------------------------------
    nfp_cpp_area_fill(semArea, 0, kSemLockValue, semSize_);
    nfp_cpp_area_release_free(semArea);

    // -------------------------------------------------------------------------
    // Step 3 & 4: Read __flow_data for slots where semaphore == 3,
    //             then fill those slots to 0.
    // -------------------------------------------------------------------------
    const uint64_t dataAddr = dataAddr_[bufferState_];
    nfp_cpp_area *dataArea = nfp_cpp_area_alloc(cpp_, dataCppId_, dataAddr, dataSize_);
    if (!dataArea) {
        // Unlock semaphores before returning on error
        semArea = nfp_cpp_area_alloc(cpp_, semCppId_, semAddr, semSize_);
        if (semArea) {
            if (nfp_cpp_area_acquire(semArea) >= 0) {
                nfp_cpp_area_fill(semArea, 0, kSemUnlockValue, semSize_);
            }
            nfp_cpp_area_release_free(semArea);
        }
        return {};
    }
    if (nfp_cpp_area_acquire(dataArea) < 0) {
        nfp_cpp_area_free(dataArea);
        // Unlock semaphores before returning on error
        semArea = nfp_cpp_area_alloc(cpp_, semCppId_, semAddr, semSize_);
        if (semArea) {
            if (nfp_cpp_area_acquire(semArea) >= 0) {
                nfp_cpp_area_fill(semArea, 0, kSemUnlockValue, semSize_);
            }
            nfp_cpp_area_release_free(semArea);
        }
        return {};
    }

    const unsigned long flowDataEntrySize = static_cast<unsigned long>(sizeof(flow_data));

    for (size_t i = 0; i < slotCount_; ++i) {
        if (semaphores[i] == kSemDataReady) {
            // Read the flow_data entry at this slot
            flow_data entry{};
            unsigned long offset = static_cast<unsigned long>(i * flowDataEntrySize);
            int readResult = nfp_cpp_area_read(dataArea, offset, &entry, flowDataEntrySize);
            if (readResult == static_cast<int>(flowDataEntrySize)) {
                result.push_back(entry);
            }

            // Clear the flow_data slot to 0
            nfp_cpp_area_fill(dataArea, offset, kDataClearValue, flowDataEntrySize);
        }
    }

    nfp_cpp_area_release_free(dataArea);

    // -------------------------------------------------------------------------
    // Step 5: Fill the _global_semaphores to 0 (unlock — tell SmartNIC we are done)
    // -------------------------------------------------------------------------
    semArea = nfp_cpp_area_alloc(cpp_, semCppId_, semAddr, semSize_);
    if (semArea) {
        if (nfp_cpp_area_acquire(semArea) >= 0) {
            nfp_cpp_area_fill(semArea, 0, kSemUnlockValue, semSize_);
        }
        nfp_cpp_area_release_free(semArea);
    }

    // -------------------------------------------------------------------------
    // Step 6: Toggle the active buffer for the next read
    // -------------------------------------------------------------------------
    bufferState_ = 1 - bufferState_;

    return result;
}
