# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

nv-monitor is a single-file C headless Prometheus exporter for the NVIDIA DGX Spark (Grace ARM CPU + GB10 Blackwell GPU). It exposes CPU, memory, GPU, NIC, RDMA, and thermal-zone metrics on an HTTP endpoint for Prometheus scraping. There is no TUI — the exporter is the only mode.

## Build

```bash
make          # builds nv-monitor binary (-O3 -march=native)
make portable # builds without -march=native (for CI/distribution)
make test     # builds and runs unit tests
make clean    # removes binaries
```

Direct compilation: `gcc -O2 -Wall -Wextra -std=gnu11 -o nv-monitor nv-monitor.c -ldl -lpthread`

Dependencies: `build-essential`

Works on both **aarch64** (DGX Spark) and **x86_64**. On x86 the per-core `type` label is `unknown` (no ARM part IDs); everything else works identically.

## Architecture

Everything is in `nv-monitor.c` (~1450 lines). Key sections:

- **NVML dynamic loading**: Loads `libnvidia-ml.so.1` via `dlopen`/`dlsym` at runtime. Uses a variadic LOAD macro to try versioned symbols first (e.g. `nvmlInit_v2` before `nvmlInit`). All NVML function pointers are prefixed with `p` (e.g. `pNvmlInit`).
- **CPU sampling**: Reads `/proc/stat` delta between consecutive scrapes to compute per-core usage percentages.
- **Memory**: Parses `/proc/meminfo` for used/available/buffers/cached/swap.
- **CPU thermals/freq**: Reads per-thermal-zone temps via `read_thermal_zones()` from `/sys/class/thermal/` and per-core frequencies from `/sys/devices/system/cpu/`.
- **Prometheus exporter**: A minimal HTTP server on a dedicated pthread, serving OpenMetrics-formatted metrics at `/metrics`. Uses POSIX sockets with `poll()` for clean shutdown. `-p PORT` is required. The main thread idles in `pause()` — **all collection happens in the scrape path**: CPU% is the delta between scrapes, network/RDMA are raw `*_total` counters read per scrape, everything else sampled fresh. There is no internal timer. Enables multi-machine monitoring via Prometheus/Grafana.

## Memory allocation (CRITICAL — do not add runtime allocations)

All memory is allocated once at startup, sized to the detected hardware. **Zero allocations occur in any per-frame or per-scrape code path.** This is a long-running application that must run for weeks with no memory growth.

- CPU arrays are dynamically sized to `sysconf(_SC_NPROCESSORS_CONF)` at startup
- GPU arrays are sized to `gpu_count` from NVML at startup
- Prometheus buffers are pre-allocated when the server thread starts
- The `compute_cpu_usage()` and `format_metrics()` functions must NEVER call malloc/calloc/realloc

If you need to add new data collection, allocate the buffer at startup alongside the existing arrays — not in the hot path. Run `./soak-test.sh 10` after any changes to verify RSS stability.

## Locale / decimal separator (CRITICAL for Prometheus)

The Prometheus exposition format **requires** decimal points (`1.23`), never commas (`1,23`). On systems with non-English locales (e.g. `es_ES.UTF-8`), `printf("%.2f")` can produce commas, which causes Prometheus scrape parse errors (`up=0`).

**Fix in code**: `main()` calls `setlocale(LC_NUMERIC, "C")` to force POSIX decimal formatting. Do **not** call `setlocale(LC_ALL, "")` or any other locale — nothing in the program needs the user locale, and it would reintroduce the comma problem.

**Do not remove** the `setlocale(LC_NUMERIC, "C")` call. Any future code that formats floats for Prometheus output depends on it.

## DGX Spark specifics

- The GB10 GPU uses **unified memory** shared with the Grace CPU. `nvmlDeviceGetMemoryInfo` returns NOT_SUPPORTED — the code detects this and omits GPU memory metrics instead of showing zeros.
- Target arch is **aarch64**. NVML library paths include both aarch64 and x86_64 fallbacks.
- No fan speed sensor on GB10 (handled gracefully via return code check).
- **Grace CPU is big.LITTLE**: 10x Cortex-X925 (performance, 3.9 GHz) + 10x Cortex-X725 (efficiency, 2.8 GHz). Core types are identified via ARM CPU part IDs in `/proc/cpuinfo` (`0xd85` = X925, `0xd87` = X725) and exposed as the per-core `type` label in Prometheus.
- When HugePages are active, `MemAvailable` is inaccurate — the code uses `HugePages_Free * Hugepagesize` instead (per NVIDIA known-issues docs).
