# Improve the time adjuster

## Goal:

Imporve the time adjuster to make the difference between unix time and Netronome SmartNIC's hardware time under 1 ms for long period.

Unix time means the Linux wall-clock value returned by `CLOCK_REALTIME`. The
human-readable `date` command formats the same underlying timestamp. Elapsed
time, sample latency, and host clock-step detection use `CLOCK_MONOTONIC_RAW`.

The pass/fail error is:

```text
error_ns = converted_smartnic_time_ns - sampled_host_realtime_ns
abs_error_ns = abs(error_ns)
```

Development qualification requires P99 absolute error below 1 ms after warm-up
for at least 1 hour. Extended qualification requires the same P99 bound for at
least 24 hours. Final acceptance requires one continuous month with P99
absolute error below 1 ms over all valid samples, excluding intervals explicitly
marked invalid because of host realtime steps or device resets.

## How Netronome SmartNIC timestamp produced

The files `./mac_time.c`, `./mac_time.h`, and `./mac_time_user.c` are the functions Netronome SmartNIC used to update it's hardware time.

To start, the Netronome SmartNIC's CPU frequence is 1GHZ, and it will use independent CPU to update it's time using `mac_time_update()`.

## How flow_data record timestamp about packet.

The Netronome SmartNIC's independent CPU will write it's current hardware timestamp into `mac_time` memory, which can be accessed by Host using lib in `#include <nfp_cpp.h>`.

The Netronome SmartNIC will also use the Micro-C code to get it's hardware time and store it into `start_time`, `end_time` in `flow_data`.

```c
static __forceinline struct mac_time_data
get_current_mac_time(void)
{
    __xread struct mac_time_state time_xfer;
    struct mac_time_state time_state;

    mac_time_fetch(&time_xfer);
    time_state = time_xfer;

    return mac_time_calc(time_state);
}
```

There are two clock paths:

1. The exported `mac_time` state, read by the host through a CPP area.
2. Packet timestamps created in firmware by `mac_time_calc()` from cached
   `mac_time_state` plus the ME timestamp counter.

Firmware is treated as read-only for this project. The host userspace process
may read runtime symbols and flow timestamps, but it must not modify firmware,
change `mac_time_update()`, add exported fields, or write calibration values to
the device.

The userspace implementation now calibrates against observed publications of
the exported `mac_time` state. Repeated stale reads are not used as fresh
calibration samples; a sample is created only when a new valid state becomes
visible. The publication time is modeled as the interval between the last host
observation of the old state and the first host observation of the new state,
and that interval is retained as sample uncertainty.

## Observation

In my observation, the current `Time` module of this project will provide a slower unix time from hardware time. That is, the `start_time` and `end_time` are **2ms** slower than unix time.

**More important, the difference will increase when the time go by.**

## Implementation

The host implementation is split into:

- `MacClockReader`: resolves and reads the exported `mac_time` runtime symbol
  through CPP areas.
- `ClockSampler`: brackets reads with host realtime and monotonic-raw clocks,
  detects publication transitions, rejects stale or implausible observations,
  and reports sample uncertainty.
- `ClockCalibration`: owns the affine offset and frequency model.
- `TimeAdjuster`: coordinates startup, refresh, status reporting, and flow
  timestamp conversion.

Production conversion remains:

```text
converted_unix_ns = unix_origin_ns
                  + round((smartnic_ns - smartnic_origin_ns) * scale)
```

Raw SmartNIC timestamps in `flow_data` remain unchanged. Conversion happens
only at the output boundary.

The diagnostic executable `raw_clock_probe` records CSV observations of host
clock bounds, CPP read latency, raw `mac_time_state` fields, validity, rejection
reason, and whether the state changed from the previous valid read. Use it for
one-hour, 24-hour, and one-month validation runs.
