# sflow_middle
middle program for sflow collector

compile process:

```bash
mkdir build
cd build
cmake ..
make
```

`MID` writes logs to the console and to `logs/sflow_middle.log`. The active
log file is limited to 10 MB; one rotated backup is retained as
`logs/sflow_middle.log.1`. The backup is created only after the active log
reaches the 10 MB limit.
