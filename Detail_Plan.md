# Detailed Plan to Improve SmartNIC Time Adjustment

## 1. Objective

Synchronize the Netronome SmartNIC clock with the host's current Linux Unix
wall-clock time, represented by `CLOCK_REALTIME` and displayed by the Linux
`date` command, so that the P99 absolute clock-mapping error remains below 1 ms
during operation lasting up to one month.

The work is complete only when:

- the exported SmartNIC `mac_time` clock and the timestamp source used by
  `flow_data.start_time` and `flow_data.end_time` are understood;
- startup offset and long-term frequency error are both corrected;
- clock resets, host clock steps, delayed reads, and bad samples do not silently
  corrupt the model;
- an automated one-month validation demonstrates P99 absolute error below 1 ms
  under the agreed workload.

## 2. Problems in the Current Plan

`Plan.md` describes the symptom and relevant firmware files, but it needs the
following additions before implementation can be considered well specified:

1. Define Unix time as Linux wall-clock time: the Unix epoch value represented
   by `CLOCK_REALTIME` and displayed in local formatted form by the Linux
   `date` command. Elapsed-time and clock-step detection should still use
   `CLOCK_MONOTONIC_RAW`.
2. Define the error requirement as P99 absolute error below 1 ms.
3. Define the longest supported and validated duration as one month, with
   shorter one-hour and 24-hour qualification tests during development.
4. Separate the two SmartNIC clock paths:
   - the exported `mac_time` state read by the host through the same CPP-area
     method used in `src/Reader.cpp`;
   - packet timestamps produced by `mac_time_calc()` from cached
     `mac_time_state` and the ME timestamp counter.
5. State that this is a clock-synchronization problem. Network transmission
   latency, packet ingress time, socket receive time, and sender timestamps are
   outside the requirement.
6. State that SmartNIC firmware is read-only for this project. The solution may
   read existing runtime symbols and timestamps but may not modify firmware,
   add exported fields, change `mac_time_update()`, or write calibration values
   to the device.
7. Include hypotheses for both the initial 2 ms offset and the increasing
   error, with an experiment that can confirm or reject each hypothesis.
8. Specify implementation stages, tests, telemetry, failure handling, and
   acceptance criteria.

## 3. Current-State Findings

### 3.1 Host calibration path

`src/Time.cpp` currently:

- reads the NBI MAC nanosecond and second registers at XPB address
  `0x0840001c`;
- brackets the register reads with `CLOCK_REALTIME` and
  `CLOCK_MONOTONIC_RAW`;
- uses the host-time midpoint as the reference time;
- rejects reads slower than 800 us;
- initializes an offset from the eight lowest-latency samples;
- fits a linear model:

  `unix_ns = unix_origin_ns + (mac_ns - mac_origin_ns) * scale`;

- refreshes the model with one sample per application read cycle.

This is a reasonable base, but it calibrates the live NBI clock. It does not
directly measure the error added when firmware creates packet timestamps through
`mac_time_calc()`. The planned implementation must replace this direct XPB
calibration read with retrieval of the exported `mac_time` symbol through the
CPP-area pattern already used by `SmartNicReader`.

### 3.2 Firmware timestamp path

The packet path:

1. `mac_time_update()` reads the NBI MAC clock and the 32-bit ME timestamp
   counter.
2. It stores a synchronization point in `mac_time_state`.
3. `mac_time_calc()` reads the current ME counter.
4. It converts the ME counter delta using `conv_mult` and `conv_rshift`.
5. It adds that delta to the cached MAC time.

Therefore, growing packet timestamp error can be caused by:

- an inaccurate ME-cycle conversion factor;
- an incorrect assumption about the actual ME clock frequency;
- truncation introduced by the multiplier and shift;
- an excessive or irregular `mac_time_update()` interval;
- incorrect rollover handling;
- an inaccurate fixed `MAC_FETCH_TIME_NS`;
- a torn or stale `mac_time_state` read;
- drift in the NBI clock itself;
- host-side sampling and regression error.

These firmware behaviors are fixed inputs. Userspace must estimate and
compensate for their combined offset and drift without changing them.

### 3.3 Immediate test inconsistency

The current `time_calibration_test` builds but fails:

`drift calibration did not become active during a short run`

The test supplies approximately 3 seconds of drift samples, while
`kMinimumRegressionSpanNs` requires 5 seconds. The implementation contract and
the test expectation must be reconciled before using this suite as a quality
gate.

## 4. Phase 1: Define the Measurement Contract

Before changing the correction algorithm, document these values in `Plan.md`
and in test configuration:

- Reference clock: host `CLOCK_REALTIME`, which is the Unix wall clock shown in
  human-readable form by the Linux `date` command. Time-zone formatting does
  not change the underlying Unix timestamp.
- Interval clock: host `CLOCK_MONOTONIC_RAW`.
- Clock-mapping error:

  `error_ns = converted_smartnic_time_ns - sampled_host_realtime_ns`

- Absolute error:

  `abs_error_ns = abs(error_ns)`

- Development qualification:
  - duration: at least 1 hour;
  - warm-up: no more than 30 seconds;
  - P99 absolute error: below 1 ms after warm-up;
  - report maximum error as diagnostic information, but it is not the primary
    pass/fail criterion.
- Extended qualification:
  - duration: at least 24 hours;
  - P99 absolute error: below 1 ms after warm-up.
- Final acceptance:
  - duration: one continuous month;
  - representative SmartNIC, CPP, and host load;
  - P99 absolute error below 1 ms, excluding periods explicitly marked invalid
    because of host realtime steps or device resets;
  - no timestamp rollback for increasing SmartNIC clock values;
  - recovery after a clock reset or host clock step within a defined period,
    recommended at 10 seconds or less.

For a sample set of absolute errors sorted in ascending order, define P99 using
the nearest-rank index `ceil(0.99 * N)` so test tools report the same result.
The final result must be calculated over all valid samples in the complete
one-month run, not by averaging daily P99 values. Also report P50, P95, P99.9,
and maximum error to expose outliers that the P99 criterion does not reject.

## 5. Phase 2: Build Diagnostic Instrumentation

### 5.1 Add a raw clock probe

Add a small diagnostic executable, separate from the production reader, that
records CSV or JSON Lines samples containing:

- host realtime before and after each device read;
- host monotonic-raw time before and after each device read;
- the exported `mac_time_state` read from SmartNIC memory;
- read round-trip latency;
- `mac_time_state` fields:
  - `mac_time_s`;
  - `mac_time_ns`;
  - `me_time`;
  - `conv_mult`;
  - `conv_rshift`;
- whether the state differs from the preceding successful read;
- the host interval in which a newly published state first became visible.

Retrieve `mac_time` using the same CPP-area lifecycle as `src/Reader.cpp`:

1. resolve the `mac_time` runtime symbol and obtain its address and domain;
2. build the CPP ID with the correct memory target, read action, and island;
3. allocate the area with `nfp_cpp_area_alloc()`;
4. acquire it with `nfp_cpp_area_acquire()`;
5. bracket `nfp_cpp_area_read()` with host realtime and monotonic-raw reads;
6. require the returned byte count to equal `sizeof(mac_time_state)`;
7. release and free the area on every success and failure path.

The `mac_time` symbol name, address, domain, and expected structure size should
be logged at startup. Because firmware cannot be changed, userspace must detect
inconsistent or torn observations by validating field ranges, rejecting
backward or implausible state transitions, and optionally requiring two
identical consecutive reads before accepting a state.

### 5.2 Measure each clock path independently

Collect simultaneous observations for:

1. host realtime versus the exported `mac_time` synchronization state;
2. held-out `mac_time_state` transitions versus the userspace affine model;
3. successive `mac_time_state` publications to estimate their update cadence,
   publication jitter, offset, and long-term drift.

For each pair, calculate:

- initial offset;
- slope or drift in ppm;
- minimum, median, P95, P99, and maximum error;
- correlation with read latency;
- correlation with time since the last `mac_time_update()`;
- correlation with SmartNIC, CPP, and host load.

This identifies where the growing error first appears. Do not tune the host
regression until this separation is available.

### 5.3 Scope of the validation

Do not introduce packet sender timestamps, packet ingress timestamps, socket
receive timestamps, PTP synchronization between machines, or network-latency
corrections. They measure different events and are not part of this goal.

The validation sample is produced only by:

1. polling the exported `mac_time_state` through CPP;
2. reading `CLOCK_REALTIME` and `CLOCK_MONOTONIC_RAW` before and after every
   CPP read;
3. retaining the host interval for the last observation of the old state;
4. detecting the first observation of a different valid state;
5. treating the state publication time as lying between those two observation
   intervals;
6. pairing the newly published SmartNIC synchronization timestamp with the
   midpoint of that transition interval and treating firmware publication
   delay as part of the observed offset;
7. using the transition width and observed publication jitter as sample
   uncertainty;
8. rejecting transition samples whose CPP latency or publication interval is
   too large.

The uncertainty of one sample is bounded by its host bracketing interval. The
calibration should prefer the lowest-latency samples and reject samples whose
uncertainty is too large.

`mac_time_s` and `mac_time_ns` describe the synchronization instant associated
with `mac_time_state.me_time`; they are not the SmartNIC time at an arbitrary
later CPP-read instant. Userspace cannot reconstruct the exact current
SmartNIC time without a current ME counter. It must therefore calibrate from
detected state publications rather than pairing every repeated, stale state
with the host read midpoint. The achievable uncertainty is bounded by polling
cadence, CPP-read latency, and firmware publication jitter.

## 6. Phase 3: Confirm or Reject Root-Cause Hypotheses

Run the following experiments in order.

### 6.1 Check the exported MAC synchronization clock

- Fit host realtime against the exported `mac_time` samples over at least one
  hour.
- If its slope is stable and its residual remains below the target, the NBI
  clock is not the main source of SmartNIC clock-mapping drift.
- If it drifts, retain host-side affine calibration and estimate its frequency
  ratio robustly.

### 6.2 Check ME-cycle conversion

- Read and log the existing `conv_mult` and `conv_rshift` fields without
  changing them.
- Use the source code and available device documentation to understand the
  expected ME conversion.
- Infer the effective end-to-end frequency ratio from long-running
  `mac_time_state` samples and host `CLOCK_REALTIME`.
- Fit the same affine ratio to representative `flow_data` timestamp ranges.
- Treat any firmware conversion error as part of the userspace model's scale
  correction.

For example, a conversion error of 1,000 ppm accumulates 1 ms per second and
would immediately violate the requirement.

### 6.3 Observe firmware update interval and rollover

- Infer the real `mac_time_update()` interval by detecting state changes from
  repeated userspace reads.
- Record the median, P95, P99, minimum, and maximum publication interval.
- Test transitions across:
  - NBI nanosecond rollover;
  - NBI second rollover;
  - ME timestamp-counter rollover.
- Detect implausible jumps that indicate rollover, reset, stale data, or a torn
  read and reset the userspace model when required.

### 6.4 Check fixed read-delay compensation

- Treat the firmware's fixed `MAC_FETCH_TIME_NS` behavior as part of the
  observed constant offset.
- Do not attempt to change or separately correct the firmware constant unless
  a stable userspace offset term already accounts for it.
- Treat CPP latency and state-publication detection delay as sample
  uncertainty, not as a firmware value to edit.

### 6.5 Check state consistency

- Verify the host structure size, field offsets, and byte order against the
  existing firmware definition.
- Add host compile-time size and offset assertions.
- Read the state twice when practical and reject observations whose fields are
  invalid, move backward unexpectedly, or change in a way that cannot represent
  one valid firmware update.

At the end of this phase, write a short root-cause report with evidence. The
userspace model must compensate for the measured combined behavior even when
the underlying cause is inside immutable firmware.

## 7. Phase 4: Implement the Userspace Clock Model

All correction is implemented in host userspace. The program reads SmartNIC
state and flow timestamps but never writes clock parameters or changes
firmware.

### 7.1 Build publication-aware samples

- Poll `mac_time_state` on a dedicated userspace schedule.
- Retain the last valid state and its host observation interval.
- Create a calibration sample only when a new valid state appears.
- Associate the new SmartNIC synchronization timestamp with the midpoint
  between the last old-state observation and first new-state observation.
- Record the full transition width as uncertainty.
- Prefer transitions observed with the smallest uncertainty.

### 7.2 Fit offset and frequency entirely in userspace

- Fit:

  `host_realtime_ns = host_origin_ns +`
  `(smartnic_ns - smartnic_origin_ns) * scale`

- Estimate startup offset from the lowest-uncertainty publication samples.
- Estimate scale from a sufficiently long rolling window.
- Center arithmetic around recent origins and use `long double` or fixed-point
  arithmetic to avoid precision loss at Unix-epoch magnitudes.
- Apply the model to `flow_data.start_time` and `flow_data.end_time`.
- Never modify the raw SmartNIC values stored in memory.

### 7.3 Make model updates stable

- Use robust outlier rejection and uncertainty weighting.
- Limit how quickly offset and scale may change between accepted models.
- Publish a new model only when sample count, span, residual, and scale checks
  pass.
- Keep the last known-good model when a candidate fit fails.
- Prevent model replacement from making converted timestamps move backward.

### 7.4 Detect immutable-source failures

- Mark calibration degraded if no new `mac_time_state` publication is observed
  within a configurable timeout.
- Reset calibration after SmartNIC timestamp rollback, device reset, or a host
  realtime step.
- Count invalid, duplicate, stale, torn, and high-uncertainty observations
  separately.

## 8. Phase 5: Structure and Harden the Userspace Implementation

### 8.1 Isolate device I/O from model fitting

Split the module into testable responsibilities:

- `MacClockReader`: obtains and validates the existing exported
  `mac_time_state` using the `nfp_cpp_area_*` method used by
  `SmartNicReader`;
- `ClockSampler`: brackets device reads with host clocks and calculates sample
  latency, detects state transitions, and calculates publication uncertainty;
- `ClockModel`: owns offset, scale, filtering, and conversion;
- `TimeAdjuster`: coordinates startup, periodic refresh, and status reporting.

Inject the reader/clock functions in tests so hardware is not required for
algorithm validation.

### 8.2 Improve sample timestamping

- Keep both host bounds for each CPP read.
- For an unchanged state, update the last interval in which that state was
  observed.
- For a changed state, derive the publication interval from the old-state and
  new-state observation bounds.
- Store interval bounds and uncertainty, not only a midpoint.
- Select transition samples by low uncertainty using a rolling distribution
  rather than a single hard-coded 800 us threshold alone.

### 8.3 Use a robust affine model

Maintain:

`unix_ns = offset + scale * mac_ns`

Implementation requirements:

- center values around a recent origin to preserve precision;
- estimate scale only after a sufficient time span;
- use robust outlier rejection based on median absolute deviation;
- require a minimum number of inliers;
- bound scale by a device-appropriate ppm limit;
- keep the last known-good model if a new fit is invalid;
- report confidence information, sample span, and residual percentiles;
- prevent a noisy refit from causing converted time to jump backward.

The startup offset model and long-term drift model should have separate states:
`warming_up`, `offset_only`, `tracking`, and `degraded`.

### 8.4 Handle discontinuities explicitly

Detect and respond to:

- NBI clock rollback or device reset;
- `CLOCK_REALTIME` steps relative to `CLOCK_MONOTONIC_RAW`;
- an implausibly large model residual;
- stale calibration samples;
- repeated device-read failure.

On a host realtime step, discard the old Unix offset and reacquire it. On a
device reset, discard both offset and scale. During reacquisition, either:

- mark converted timestamps as temporarily untrusted; or
- continue with the last model while exposing a degraded status.

Choose and document one behavior.

### 8.5 Set refresh cadence independently

Calibration is currently refreshed as part of the once-per-second SmartNIC
reader loop. Move it to a dedicated schedule or make the interval configurable
so slow flow reads do not delay calibration.

Recommended initial values:

- startup: 64 to 128 rapid samples;
- polling: frequent enough to observe state changes with sub-millisecond
  uncertainty when CPP load permits;
- refresh: publish a model only when a new valid state transition is available;
- regression window: initially 5 to 15 minutes, then tune from one-hour and
  24-hour traces;
- model publication: only after fit-quality checks pass.

Tune these from recorded data rather than treating them as final constants.

## 9. Detailed Implementation Blueprint

This section turns the phase plan into concrete code work. The implementation
should be completed in small commits so the current direct-XPB calibration path
can remain available until the exported-`mac_time` model passes shadow-mode
validation.

### 9.1 Target architecture

Replace the direct NBI-register sampling in `TimeAdjuster::readSample()` with a
read-only exported-symbol sampling pipeline:

```text
SmartNIC exported mac_time_state
        |
        v
MacClockReader::readState()
        |
        v
ClockSampler::poll()
        |
        v
PublicationTransitionDetector
        |
        v
ClockModel::addSample()
        |
        v
TimeAdjuster::toUnixNanoseconds(flow timestamp)
```

Keep `ClockCalibration` or replace it with `ClockModel`, but the final
responsibilities must be separated:

- `MacClockReader` performs only CPP symbol resolution and state reads.
- `ClockSampler` performs host-clock bracketing and latency accounting.
- `PublicationTransitionDetector` converts repeated polls into publication
  samples.
- `ClockModel` fits, validates, and applies the affine model.
- `TimeAdjuster` owns lifecycle, configuration, status, and flow conversion.

The production conversion remains:

```text
converted_unix_ns = unix_origin_ns
                  + round((smartnic_ns - smartnic_origin_ns) * scale)
```

where `smartnic_ns` is the raw `flow_data.start_time` or
`flow_data.end_time` value composed from its seconds and nanoseconds fields.

### 9.2 Data structures

Add host-side structures that preserve all timing uncertainty instead of
collapsing every read to one midpoint too early.

```cpp
struct HostClockBounds {
    int64_t realtime_before_ns;
    int64_t realtime_after_ns;
    int64_t monotonic_before_ns;
    int64_t monotonic_after_ns;
};

struct MacTimeStateHost {
    uint32_t mac_time_s;
    uint32_t mac_time_ns;
    uint32_t me_time;
    uint16_t conv_mult;
    uint16_t conv_rshift;
};

struct MacStateObservation {
    MacTimeStateHost state;
    HostClockBounds bounds;
    int64_t read_latency_ns;
    bool valid;
    std::string reject_reason;
};

struct PublicationSample {
    int64_t smartnic_ns;
    int64_t host_realtime_ns;
    int64_t host_monotonic_ns;
    int64_t uncertainty_ns;
    int64_t transition_start_mono_ns;
    int64_t transition_end_mono_ns;
    uint32_t me_time;
    uint16_t conv_mult;
    uint16_t conv_rshift;
};
```

Use `MacTimeStateHost` instead of reusing firmware headers directly in public
interfaces. Add `static_assert(sizeof(MacTimeStateHost) == 16)` and offset
assertions in the `.cpp` file that performs the raw read. Treat any host-side
size or offset mismatch as a build failure.

### 9.3 `MacClockReader`

Create `include/MacClockReader.hpp` and `src/MacClockReader.cpp`.

Constructor inputs:

- `nfp_cpp *cpp`;
- runtime symbol name, default `mac_time`;
- optional expected structure size, default `sizeof(MacTimeStateHost)`.

Startup behavior:

1. resolve the symbol with the same runtime-symbol mechanism already used by
   the application for other exported addresses;
2. store the resolved address, domain, island, and CPP ID;
3. log symbol name, address, domain, island, and structure size;
4. fail construction if the symbol cannot be resolved.

Read behavior:

1. allocate an area with `nfp_cpp_area_alloc()`;
2. acquire it with `nfp_cpp_area_acquire()`;
3. read exactly `sizeof(MacTimeStateHost)` bytes with
   `nfp_cpp_area_read()`;
4. release and free the area on all paths;
5. return a typed state only if the byte count is exact.

Validation rules:

- reject `mac_time_ns >= 1000000000`;
- reject `conv_rshift >= 32`;
- reject `conv_mult == 0`;
- reject all-zero state after startup grace unless diagnostics prove it is a
  valid firmware state;
- reject backward `mac_time_s:mac_time_ns` transitions unless a reset is being
  declared;
- reject transitions whose `me_time` delta and MAC-time delta are impossible
  for the configured ME frequency tolerance;
- count every rejection by reason.

If torn reads appear in diagnostics, add an optional stable-read mode:

1. read state A;
2. read state B immediately;
3. accept if A and B are byte-identical;
4. otherwise retry up to a small configured limit and record a torn-read
   rejection if no stable pair is found.

### 9.4 `ClockSampler`

Create `ClockSampler` around an injected `MacClockReader` and injected clock
function. The injected clock function allows deterministic tests without NFP
hardware.

Polling sequence:

1. read `CLOCK_REALTIME`;
2. read `CLOCK_MONOTONIC_RAW`;
3. read `mac_time_state`;
4. read `CLOCK_MONOTONIC_RAW`;
5. read `CLOCK_REALTIME`;
6. compute read latency from monotonic bounds;
7. discard the observation if either host clock moves backward.

Do not pair a stale `mac_time_state` with the current host midpoint as a
calibration sample. Stale observations only update the interval in which the
old state was still visible.

### 9.5 Publication transition detector

The detector owns the previous accepted observation and emits a
`PublicationSample` only when the exported state changes.

For repeated state:

- update `last_seen_same_state_bounds`;
- do not call `ClockModel::addSample()`;
- update duplicate and stale counters.

For changed state:

1. validate that the new state is plausible relative to the previous state;
2. build the publication interval from the last old-state observation and the
   first new-state observation;
3. set `host_realtime_ns` to the midpoint of the realtime interval;
4. set `host_monotonic_ns` to the midpoint of the monotonic interval;
5. set `uncertainty_ns` to the monotonic interval width plus the new read
   latency;
6. set `smartnic_ns` from `mac_time_s` and `mac_time_ns`;
7. reject the transition if uncertainty exceeds the configured threshold;
8. pass accepted transition samples to the model.

Use the transition interval, not the single CPP-read midpoint, because the
firmware state was published sometime after the last observation of the old
state and before the first observation of the new state.

### 9.6 Startup acquisition

Startup should reach an `offset_only` model quickly but should not pretend that
frequency drift is known.

Algorithm:

1. poll rapidly until either `startup_sample_count` publication samples are
   accepted or `startup_timeout` expires;
2. sort accepted samples by `uncertainty_ns`;
3. keep the best `startup_best_sample_count`;
4. set `smartnic_origin_ns` to the best sample's SmartNIC timestamp;
5. set `unix_origin_ns` to
   `smartnic_origin_ns + median(host_realtime_ns - smartnic_ns)`;
6. set `scale = 1.0`;
7. enter `offset_only`.

If the startup timeout expires with too few publication samples, expose
`degraded` status and either fail construction or continue without conversion
according to the deployment setting. The recommended default is to fail fast in
tools that require accurate timestamps and to continue degraded in diagnostics.

### 9.7 Rolling affine fit

The drift fit consumes only accepted `PublicationSample` values.

Windowing:

- keep samples in monotonic-time order;
- remove samples older than `regression_window_ns`;
- require `minimum_regression_span_ns`;
- require `minimum_regression_samples`;
- prefer low-uncertainty samples if the window grows above the configured
  maximum sample count.

Fit:

1. choose origins near the first inlier in the current window;
2. use `long double` centered arithmetic for `x = smartnic_ns - origin`;
3. perform a first least-squares fit;
4. compute residuals;
5. compute median and median absolute deviation;
6. reject outliers above `max(residual_floor_ns, mad_multiplier * mad)`;
7. perform a second fit on inliers;
8. calculate residual P50, P95, P99, and maximum;
9. calculate drift as `(scale - 1.0) * 1e6` ppm.

Candidate model checks:

- sample span is sufficient;
- inlier count is sufficient;
- absolute drift is below configured ppm limit;
- residual P99 is below configured threshold;
- model does not create a large step at the latest converted timestamp;
- model does not make converted time move backward for increasing SmartNIC
  timestamps;
- model age is newer than the last accepted model.

If all checks pass, atomically publish the candidate and enter `tracking`. If
any check fails, keep the last known-good model and increment the appropriate
rejection counter.

### 9.8 Host realtime step detection

Track the relationship between `CLOCK_REALTIME` and `CLOCK_MONOTONIC_RAW` on
every observation:

```text
host_step_error =
    (realtime_now - realtime_previous)
  - (monotonic_raw_now - monotonic_raw_previous)
```

If `abs(host_step_error)` exceeds `host_step_threshold_ns`:

1. increment the host-step counter;
2. mark the current calibration interval invalid;
3. discard the Unix offset immediately;
4. keep SmartNIC frequency history only if the step is a pure realtime offset
   change and the SmartNIC state did not reset;
5. reacquire startup offset samples;
6. publish `offset_only` before returning to `tracking`.

The validation report must exclude only the explicitly marked invalid interval,
not arbitrary data around it.

### 9.9 Device reset and rollback handling

Declare a device discontinuity when any of these is observed:

- `mac_time_s:mac_time_ns` moves backward beyond rollover rules;
- `mac_time_s:mac_time_ns` jumps forward by more than the configured maximum
  publication interval;
- `conv_mult` or `conv_rshift` changes unexpectedly;
- repeated reads return all-zero or invalid state after the device had been
  valid;
- flow timestamps move backward relative to previously converted flow
  timestamps.

On device discontinuity:

1. increment reset counter;
2. clear all publication samples;
3. discard both offset and scale;
4. enter `warming_up`;
5. reacquire startup samples;
6. publish a new model only after startup checks pass.

Converted timestamps during reacquisition should be marked untrusted. If the
application must continue outputting timestamps, include a status field or log
record that identifies the degraded interval.

### 9.10 Flow timestamp conversion integration

Keep raw SmartNIC timestamps intact in `flow_data`. Convert only at the output
boundary where the application currently presents Unix time.

Implementation steps:

1. compose raw SmartNIC nanoseconds from `start_time.sec`,
   `start_time.nsec`, `end_time.sec`, and `end_time.nsec`;
2. reject nanoseconds outside `[0, 1000000000)`;
3. call `TimeAdjuster::toUnixNanoseconds()`;
4. attach calibration status to diagnostic output;
5. if calibration is degraded, either emit raw timestamps plus status or emit
   converted timestamps explicitly marked degraded.

Do not calibrate from packet receive time, sender time, or socket timestamp.
Those are different events and would hide the SmartNIC clock behavior that this
project needs to correct.

### 9.11 Shadow mode

Before enabling the new model by default, run both conversions:

- old direct-XPB `TimeAdjuster` model;
- new exported-`mac_time_state` publication-aware model.

For every converted flow timestamp, log:

- raw SmartNIC seconds and nanoseconds;
- old converted Unix nanoseconds;
- new converted Unix nanoseconds;
- difference between old and new conversion;
- new model state and drift ppm;
- current sample uncertainty and residual summary.

Shadow mode is complete when the new model is stable under idle load,
representative traffic, and heavy CPP contention, and when the old/new
difference is understood from the raw diagnostic samples.

### 9.12 File-level change list

Expected source changes:

- `include/Time.hpp`: replace direct XPB-address constructor options with
  exported-symbol and calibration configuration options; extend status fields.
- `src/Time.cpp`: remove direct NBI register reads from production sampling;
  keep the old path only behind a shadow-mode or diagnostic flag.
- `include/MacClockReader.hpp` and `src/MacClockReader.cpp`: add exported
  `mac_time_state` reader.
- `include/ClockSampler.hpp` and `src/ClockSampler.cpp`: add host-clock
  bracketing and transition sample construction.
- `test/time_calibration_test.cpp`: update span expectations and add model
  discontinuity tests.
- `test/clock_sampler_test.cpp`: add deterministic publication-transition
  tests with fake reader and fake clocks.
- `setting.yaml`: add the configuration keys listed in Phase 6.
- `CMakeLists.txt`: build the new implementation files, unit tests, and raw
  diagnostic executable.

Keep `mac_time.c`, `mac_time.h`, and `mac_time_user.c` unchanged except for
comments in local documentation. Firmware behavior is an input to the host
model, not part of the implementation surface.

### 9.13 Implementation milestones

1. Add model/status enums and configuration structures without changing
   behavior.
2. Add `MacClockReader` and a diagnostic tool that dumps raw observations.
3. Add `ClockSampler` and publication-transition tests.
4. Replace direct-XPB production samples with publication samples behind a
   feature flag.
5. Implement startup `offset_only` acquisition.
6. Implement rolling `tracking` fit with robust rejection.
7. Add host-step and device-reset recovery.
8. Add shadow-mode logging.
9. Enable the new path by default after one-hour and 24-hour qualification.
10. Run and archive the one-month acceptance report.

## 10. Phase 6: Configuration and Observability

Add YAML settings for:

- exported `mac_time` runtime-symbol name;
- state polling interval;
- calibration refresh interval;
- startup sample count;
- burst size;
- maximum accepted read latency;
- regression window and minimum span;
- maximum permitted drift in ppm;
- residual threshold;
- stale-model timeout;
- diagnostic logging interval.

Expose at least:

- calibration state;
- estimated drift in ppm;
- offset in nanoseconds;
- sample count and sample span;
- accepted and rejected sample counts by reason;
- observed `mac_time_state` publication interval and publication uncertainty;
- minimum and P99 device-read latency;
- median and P99 fit residual;
- age of the latest accepted sample;
- number of resets and host-clock steps;
- P50, P95, P99, P99.9, and maximum direct clock-mapping error.

Rate-limit warnings. A rejected burst should not print once per second
indefinitely without an aggregate count.

## 11. Phase 7: Test Plan

### 11.1 Deterministic unit tests

Expand `test/time_calibration_test.cpp` to cover:

- stable offset-only conversion;
- positive and negative drift;
- large Unix epochs without precision loss;
- low-latency sample selection;
- delayed-read outliers;
- clustered and isolated residual outliers;
- insufficient span for a scale estimate;
- invalid or excessive scale;
- host realtime step;
- device clock rollback;
- stale model behavior;
- monotonic model publication;
- nanosecond and second rollover;
- ME 32-bit counter rollover;
- exact boundary values such as `nsec == 999999999`;
- duplicate-state polling and state-transition interval construction;
- torn or implausible state rejection;
- recovery after reset.

Resolve the current 3-second-versus-5-second test contradiction by making the
required span an explicit test fixture input or by supplying samples that
satisfy the production threshold.

### 11.2 Property and simulation tests

Generate long synthetic traces with:

- known drift from -1,000 to +1,000 ppm;
- randomized latency and asymmetric read delay;
- periodic load spikes;
- oscillator noise;
- clock steps and resets;
- 24-hour, multi-day, and one-month simulated durations.

Assert that:

- the fitted scale converges within a defined ppm tolerance;
- conversion error remains within budget after warm-up;
- bad samples cannot move the model beyond a bounded amount;
- converted time never moves backward for increasing SmartNIC timestamps
  unless a reset is explicitly reported.

### 11.3 Hardware integration tests

Run at least these scenarios:

1. idle SmartNIC and host;
2. representative SmartNIC data-path load;
3. high host CPU and memory load;
4. concurrent heavy CPP access;
5. test across NBI second rollover;
6. test across ME counter rollover;
7. firmware restart or SmartNIC reset;
8. host realtime adjustment using the deployment's actual time service;
9. one-hour qualification;
10. 24-hour extended qualification;
11. one-month final acceptance run.

Store raw samples and summary statistics as test artifacts. For the one-month
run, use mergeable fixed-boundary histograms or another deterministic
quantile-summary format so the overall month-wide P99 can be calculated without
keeping every sample in memory. Retain enough raw data around resets, clock
steps, and error spikes for diagnosis.

## 12. Phase 8: Integration and Rollout

1. Add diagnostics without changing timestamp output.
2. Collect baseline traces and identify the root cause.
3. Implement the publication-aware userspace clock model behind a configuration
   flag.
4. Run old and new conversion models in shadow mode and log their difference.
5. Confirm that the implementation performs no SmartNIC clock writes and
   requires no firmware changes.
6. Complete unit, simulation, one-hour, and 24-hour hardware qualification
   tests.
7. Enable the new model as default.
8. Run the one-month acceptance test.
9. Remove the old path only after the new path has passed and rollback
   instructions are documented.

## 13. Deliverables

- Updated `Plan.md` with definitions, clock-path diagram, and acceptance
  criteria.
- Raw-clock diagnostic tool and documented output format.
- Root-cause measurement report.
- Refactored and configurable host time-calibration module.
- Read-only CPP `mac_time_state` sampler and publication-transition detector.
- Calibration health logging/metrics.
- Deterministic unit and simulation tests.
- Hardware qualification script, 24-hour qualification report, and one-month
  acceptance report.
- Operational documentation covering startup, degraded mode, reset recovery,
  and expected drift/residual values.

## 14. Recommended Execution Order

1. Fix the inconsistent existing calibration test.
2. Add the read-only `mac_time_state` probe and transition diagnostics.
3. Capture a one-hour baseline under idle and loaded conditions using exported
   `mac_time` reads through the `Reader.cpp` CPP-area method.
4. Measure publication cadence, offset, drift, and sample uncertainty.
5. Implement and verify the userspace affine calibration.
6. Integrate it with flow timestamp conversion.
7. Add configuration, health status, and rate-limited reporting.
8. Run deterministic and simulated long-duration tests.
9. Run shadow-mode hardware qualification.
10. Complete the one-month acceptance test and document P50, P95, P99, P99.9,
    maximum error, invalid intervals, and recovery events.

This order treats firmware behavior as immutable and compensates for its
observable offset and drift entirely in userspace while providing evidence that
the one-month P99 target is met.
