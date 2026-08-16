# corral performance

Machine: Intel Xeon Processor (Skylake, IBRS, no TSX), 2 CPUs, Ubuntu 26.04 LTS, kernel 7.0.0-31-generic.

## Termination latency

`verified_ms - trigger_ms` from the audit record: the time from the trigger until nothing is left. F6 ignores SIGTERM, so its latency includes the 1 s grace period.

| Scenario | Mode | P50 (ms) | P95 (ms) | Runs |
|---|---|---|---|---|
| F5 | CE | 9 | 12 | 30 |
| F5 | CF | 10 | 12 | 30 |
| F6 | CE | 1011 | 1013 | 30 |
| F6 | CF | 1010 | 1014 | 30 |
| F10 | CE | 9 | 12 | 30 |
| F10 | CF | 8 | 12 | 30 |

## Overhead

Wall time per run of `/bin/true`, including process creation, measured from Python.

| Runner | P50 (ms) | P95 (ms) | Runs |
|---|---|---|---|
| direct | 0.83 | 1.88 | 200 |
| CE | 11.33 | 19.47 | 200 |
| CF | 6.64 | 10.01 | 200 |
