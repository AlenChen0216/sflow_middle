# Improve the time adjuster

## Goal:

Imporve the time adjuster to make the difference between unix time and Netronome SmartNIC's hardware time under 1 ms for long period.

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

## Observation

In my observation, the current `Time` module of this project will provide a slower unix time from hardware time. That is, the `start_time` and `end_time` are **2ms** slower than unix time.

**More important, the difference will increase when the time go by.**

