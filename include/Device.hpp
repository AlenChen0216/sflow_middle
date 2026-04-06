#pragma once

#include <stdexcept>

extern "C" {
#include <nfp.h>      /* nfp_device_open / nfp_device_cpp / nfp_device_close */
#include <nfp_cpp.h>  /* nfp_cpp_read / nfp_cpp_write                        */
}

/* =========================================================================
 * NFPDevice – RAII wrapper around nfp_device + nfp_cpp handles
 * ========================================================================= */

class NFPDevice {
public:
    explicit NFPDevice(unsigned int devnum);
    ~NFPDevice();

    /* Non-copyable – unique ownership of hardware handle */
    NFPDevice(const NFPDevice &)             = delete;
    NFPDevice &operator=(const NFPDevice &)  = delete;
    NFPDevice(NFPDevice &&)                  = delete;
    NFPDevice &operator=(NFPDevice &&)       = delete;

    nfp_cpp *cpp() const noexcept { return cpp_; }
    nfp_device *dev() const noexcept { return dev_; }

private:
    nfp_device *dev_ = nullptr;
    nfp_cpp    *cpp_ = nullptr;
};