# sflow_middle

`MID` collects flow and counter snapshots from one or more SmartNICs and sends
them to one sFlow destination. It writes logs to the console and to
`logs/sflow_middle.log`. The active log file is limited to 10 MB; one rotated
backup is retained as `logs/sflow_middle.log.1`.

## Use guide

1. use "./start.sh 0 1" in sFlow_SmartNIC_offload to run the program in SmartNIC.
2. use "./start.sh" in sflow_middle to run the collector program.

## Build

```bash
mkdir build
cd build
cmake ..
make
```

## Configuration

`setting.yaml` must contain a non-empty YAML sequence of unique, unsigned
device numbers. For one card, use `devices: [0]`; for two cards, use
`devices: [0, 1]`. The legacy `devnum` key is rejected, even if `devices` is
also present. Invalid configuration is fatal and is detected before any device
is opened.

`targetIp` and `targetPort`, symbol names, timing settings, `debug`, and
`runSecs` apply to the whole process. All devices share that one destination;
per-device destinations are not supported. The example deliberately omits
`calibration_poll_interval_us`, `residual_threshold_ns`,
`stale_model_timeout_ms`, and `diagnostic_logging_interval_ms` because they
are not consumed by the application.

## Runtime behavior

At startup, MID opens every configured device, validates its symbols, and
calibrates its clock before starting any worker. A failure for any device stops
startup without running a partial service.

Each device has its own device handle, readers, clock model, and polling
worker. Each worker reads flow data before counter data and publishes complete
single-device batches. One shared transmitter thread and UDP socket consume
those batches and send all payloads to the configured shared destination.

Every emitted payload has a root-level `devnum`:

```json
{"devnum": 0, "flowMap": [{"src_ip": 1, "dst_ip": 2}]}
{"devnum": 0, "counterMap": [{"agent_ip": 1, "if_idx": 2}]}
{"devnum": 0, "execution_time": 0.01, "flowMap": [], "counterMap": []}
```

The first two forms are flow and counter UDP payloads. The final form is one
debug JSON Lines record for a complete device batch when `debug: true`.

On a signal, configured runtime deadline, or worker failure, MID requests a
common stop. Workers wake promptly, finish their producer obligations, and the
transmitter drains queued batches before exit. Signals and normal deadlines
exit successfully; configuration, initialization, worker, transmitter, and
thread-launch failures exit nonzero.
