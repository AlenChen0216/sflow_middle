#include "../include/Device.hpp"

#include <cerrno>
#include <cstring>
#include <string>

NFPDevice::NFPDevice(unsigned int devnum) {
    dev_ = nfp_device_open(devnum);
    if (!dev_) {
        throw std::runtime_error(
            std::string("nfp_device_open failed: ") + std::strerror(errno));
    }

    cpp_ = nfp_device_cpp(dev_);
    if (!cpp_) {
        nfp_device_close(dev_);
        dev_ = nullptr;
        throw std::runtime_error(
            std::string("nfp_device_cpp failed: ") + std::strerror(errno));
    }
    int symbolCount = nfp_rtsym_count(dev_);
    for (int i = 0; i < symbolCount; ++i) {
        const nfp_rtsym *rtsym = nfp_rtsym_get(dev_, i);
        if (rtsym) {
            symbolCache_[rtsym->name] = rtsym;
        }
    }
}

NFPDevice::~NFPDevice() {
    if (dev_) {
        nfp_device_close(dev_);
        dev_ = nullptr;
    }
}

const nfp_rtsym* NFPDevice::getSymbolData(const char* symbolName) const {
    if (!dev_ || !symbolName) {
        return nullptr;
    }

    auto it = symbolCache_.find(symbolName);
    if (it != symbolCache_.end()) {
        return it->second;
    }
    return nullptr; // Symbol not found
}