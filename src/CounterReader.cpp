#include "../include/CounterReader.hpp"

#include <chrono>
#include <thread>

namespace {
constexpr uint32_t kCppTargetMem = 7;
constexpr uint32_t kCppActionWrite = 32;
constexpr uint32_t kSemUnlockValue = 0;
constexpr uint32_t kDataClearValue = 0;
constexpr uint32_t kSemDataReady = 3;

static_assert(sizeof(stored_counter) == 32, "stored_counter wire size changed");
}  // namespace

CounterReader::CounterReader(nfp_cpp *cpp,
                             uint64_t staAddr, uint64_t staIsland,
                             uint64_t proAddr, uint64_t proIsland,
                             uint64_t semAddr0, uint64_t semAddr1,
                             uint64_t dataAddr0, uint64_t dataAddr1,
                             uint32_t semIsland0, uint32_t dataIsland0,
                             uint32_t semIsland1, uint32_t dataIsland1,
                             size_t slotCount)
    : cpp_(cpp), slotCount_(slotCount)
{
    staAddr_ = staAddr;
    proAddr_ = proAddr;
    semAddr_[0] = semAddr0;
    semAddr_[1] = semAddr1;
    dataAddr_[0] = dataAddr0;
    dataAddr_[1] = dataAddr1;

    semSize_ = static_cast<unsigned long>(slotCount_ * sizeof(uint32_t));
    dataSize_ = static_cast<unsigned long>(slotCount_ * sizeof(stored_counter));

    staCppId_ = NFP_CPP_ISLAND_ID(kCppTargetMem, kCppActionWrite, 0, staIsland);
    proCppId_ = NFP_CPP_ISLAND_ID(kCppTargetMem, kCppActionWrite, 0, proIsland);
    semCppId_[0] = NFP_CPP_ISLAND_ID(kCppTargetMem, kCppActionWrite, 0, semIsland0);
    dataCppId_[0] = NFP_CPP_ISLAND_ID(kCppTargetMem, kCppActionWrite, 0, dataIsland0);
    semCppId_[1] = NFP_CPP_ISLAND_ID(kCppTargetMem, kCppActionWrite, 0, semIsland1);
    dataCppId_[1] = NFP_CPP_ISLAND_ID(kCppTargetMem, kCppActionWrite, 0, dataIsland1);
}

std::vector<counter_data> CounterReader::readCounterData() noexcept {
    // SmartNicReader has already changed the shared __cur_state for this
    // sample. Read the opposite (retired) bank so flow and counter data come
    // from one synchronized snapshot; do not flip the shared state twice.
    if (!cpp_ || slotCount_ == 0) {
        return {};
    }

    std::vector<counter_data> result;

    nfp_cpp_area *staArea = nfp_cpp_area_alloc(
        cpp_, staCppId_, staAddr_, sizeof(uint32_t));
    if (!staArea) {
        return {};
    }
    if (nfp_cpp_area_acquire(staArea) < 0) {
        nfp_cpp_area_free(staArea);
        return {};
    }

    uint32_t staValue = 0;
    const int staBytesRead = nfp_cpp_area_read(
        staArea, 0, &staValue, sizeof(staValue));
    nfp_cpp_area_release_free(staArea);
    if (staBytesRead != static_cast<int>(sizeof(staValue))) {
        return {};
    }

    const int bufferState = 1 - static_cast<int>(staValue & 1);

    nfp_cpp_area *proArea = nfp_cpp_area_alloc(
        cpp_, proCppId_, proAddr_, sizeof(uint32_t) * 2);
    if (!proArea) {
        return {};
    }
    if (nfp_cpp_area_acquire(proArea) < 0) {
        nfp_cpp_area_free(proArea);
        return {};
    }
    while (true) {
        uint32_t proValue = 0;
        const int proBytesRead = nfp_cpp_area_read(
            proArea,
            bufferState * sizeof(uint32_t),
            &proValue,
            sizeof(proValue));
        if (proBytesRead != static_cast<int>(sizeof(proValue))) {
            nfp_cpp_area_release_free(proArea);
            return {};
        }
        if (proValue == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    nfp_cpp_area_release_free(proArea);

    const uint64_t semAddr = semAddr_[bufferState];
    const uint32_t semCppId = semCppId_[bufferState];
    const uint64_t dataAddr = dataAddr_[bufferState];
    const uint32_t dataCppId = dataCppId_[bufferState];

    nfp_cpp_area *semArea = nfp_cpp_area_alloc(
        cpp_, semCppId, semAddr, semSize_);
    if (!semArea) {
        return {};
    }
    if (nfp_cpp_area_acquire(semArea) < 0) {
        nfp_cpp_area_free(semArea);
        return {};
    }

    std::vector<uint32_t> semaphores(slotCount_, 0);
    const int semBytesRead = nfp_cpp_area_read(
        semArea, 0, semaphores.data(), semSize_);
    if (semBytesRead != static_cast<int>(semSize_)) {
        nfp_cpp_area_release_free(semArea);
        return {};
    }

    std::vector<size_t> haveData;
    for (size_t i = 0; i < slotCount_; ++i) {
        if (semaphores[i] == kSemDataReady) {
            haveData.push_back(i);
        }
    }

    nfp_cpp_area *dataArea = nfp_cpp_area_alloc(
        cpp_, dataCppId, dataAddr, dataSize_);
    if (!dataArea) {
        nfp_cpp_area_release_free(semArea);
        return {};
    }
    if (nfp_cpp_area_acquire(dataArea) < 0) {
        nfp_cpp_area_free(dataArea);
        nfp_cpp_area_release_free(semArea);
        return {};
    }

    for (size_t index : haveData) {
        stored_counter data{};
        const int dataBytesRead = nfp_cpp_area_read(
            dataArea,
            index * sizeof(stored_counter),
            &data,
            sizeof(data));
        if (dataBytesRead != static_cast<int>(sizeof(data))) {
            continue;
        }
        counter_data entry{};
        entry.data = data;
        entry.index = static_cast<uint32_t>(index);
        result.push_back(entry);
    }

    // Clear payload first and publish empty semaphore slots last, matching
    // Reader.cpp's ordering.
    nfp_cpp_area_fill(dataArea, 0, kDataClearValue, dataSize_);
    nfp_cpp_area_fill(semArea, 0, kSemUnlockValue, semSize_);

    nfp_cpp_area_release_free(dataArea);
    nfp_cpp_area_release_free(semArea);
    return result;
}
