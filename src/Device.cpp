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
}

NFPDevice::~NFPDevice() {
    if (dev_) {
        nfp_device_close(dev_);
        dev_ = nullptr;
    }
}
