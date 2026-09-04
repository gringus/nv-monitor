# nv-monitor

Prometheus/OpenMetrics exporter for NVIDIA GPU systems — a single <80KB binary with zero runtime dependencies. Built for the **DGX Spark** (Grace + GB10), works on any Linux system with an NVIDIA GPU.

Accurately monitor a single machine or an entire cluster with minimal overhead. Reports metrics to NVIDIA specifications via NVML, with correct handling of unified memory, HugePages, and ARM big.LITTLE core topology. Includes `demo-load`, a zero-dependency synthetic CPU/GPU load generator for validating your monitoring pipeline end-to-end.

![C](https://img.shields.io/badge/lang-C-blue) ![License](https://img.shields.io/badge/license-MIT-green) ![Arch](https://img.shields.io/badge/arch-aarch64%20%7C%20x86__64-orange) ![Build](https://github.com/gringus/nv-monitor/actions/workflows/build.yml/badge.svg)

## Design

- Scrape-driven: every scrape samples the sensors fresh — CPU% is the delta since the previous scrape, network/RDMA are raw `*_total` counters (use `rate()` in PromQL). No internal timer.
- NVML loaded dynamically at runtime — no hard dependency on NVIDIA drivers
- Correctly handles **HugePages** on DGX Spark where `MemAvailable` is inaccurate
- Reports metrics to NVIDIA specifications via NVML, with correct handling of unified memory and ARM big.LITTLE core topology (per-core `type` labels: **X925** performance / **X725** efficiency on Grace)

## Download

There's a [binary release](releases) built on every release via GitHub CI/CD pipelines.

## Building

Requires `gcc`:

```bash
sudo apt install build-essential
make
```

## Usage

```bash
./nv-monitor -p 9101                   # Prometheus metrics on :9101
```

Or install system-wide:

```bash
sudo make install
```

### Command-line options

| Flag       | Description                                | Default |
|------------|--------------------------------------------|---------|
| `-p PORT`  | Expose Prometheus metrics on PORT          | required |
| `-t TOKEN` | Require Bearer token for `/metrics`        | off     |
| `-v`       | Show version                               |         |
| `-h`       | Show help                                  |         |

## Prometheus Metrics

`-p PORT` exposes a Prometheus-compatible metrics endpoint:

```bash
./nv-monitor -p 9101              # metrics at http://localhost:9101/metrics
curl -s localhost:9101/metrics     # Check it works
```

### Available metrics

| Metric | Type | Labels | Description |
|--------|------|--------|-------------|
| `nv_build_info` | gauge | `version`, `driver` | nv-monitor version and NVIDIA driver version |
| `nv_uptime_seconds` | gauge | | System uptime |
| `nv_load_average` | gauge | `interval` | Load average (1m, 5m, 15m) |
| `nv_cpu_usage_percent` | gauge | `cpu`, `type` | Per-core CPU utilization (type = ARM core: X925, X725, etc.; `unknown` on x86) |
| `nv_cpu_seconds_total` | counter | `mode` | Cumulative CPU time per mode (`user`, `system`, `iowait`, `steal`, …) — `rate()` for iowait/steal trends |
| `nv_thermal_zone_temperature_celsius` | gauge | `zone`, `type` | Per-thermal-zone temperature (e.g. cpu-therm, GPU-therm) |
| `nv_cpu_frequency_mhz` | gauge | `cpu`, `type` | Per-core CPU frequency (type = ARM core, `unknown` on x86) |
| `nv_memory_total_bytes` | gauge | | Total system memory |
| `nv_memory_used_bytes` | gauge | | Application memory used |
| `nv_memory_bufcache_bytes` | gauge | | Buffer and cache memory |
| `nv_swap_total_bytes` | gauge | | Total swap |
| `nv_swap_used_bytes` | gauge | | Swap used |
| `nv_network_receive_bytes_total`, `nv_network_transmit_bytes_total` | counter | `device` | Per-interface traffic (loopback skipped) |
| `nv_network_receive_packets_total`, `nv_network_transmit_packets_total` | counter | `device` | Per-interface packet counts |
| `nv_network_receive_errors_total`, `nv_network_transmit_errors_total` | counter | `device` | Per-interface link errors |
| `nv_network_receive_dropped_total`, `nv_network_transmit_dropped_total` | counter | `device` | Per-interface dropped packets |
| `nv_nic_asic_temperature_celsius` | gauge | | Hottest NIC ASIC (mlx5/ConnectX hwmon) |
| `nv_drive_temperature_celsius` | gauge | `device` | NVMe/HDD temperature (hwmon, e.g. `device="nvme0"`) |
| `nv_disk_total_bytes` | gauge | `mountpoint`, `device`, `fstype` | Filesystem total size (per real mount) |
| `nv_disk_used_bytes` | gauge | `mountpoint`, `device`, `fstype` | Filesystem used bytes |
| `nv_disk_avail_bytes` | gauge | `mountpoint`, `device`, `fstype` | Filesystem available bytes |
| `nv_disk_reads_completed_total`, `nv_disk_writes_completed_total` | counter | `device` | Completed I/O ops per physical disk (partitions/virtual excluded) |
| `nv_disk_read_bytes_total`, `nv_disk_written_bytes_total` | counter | `device` | Throughput bytes per physical disk |
| `nv_gpu_info` | gauge | `gpu`, `name`, `uuid` | GPU identity (uuid omitted where unsupported, e.g. Jetson) |
| `nv_gpu_utilization_percent` | gauge | `gpu` | GPU compute utilization |
| `nv_gpu_memory_utilization_percent` | gauge | `gpu` | GPU memory controller utilization (reads 0 on unified memory) |
| `nv_gpu_temperature_celsius` | gauge | `gpu` | GPU temperature |
| `nv_gpu_power_watts` | gauge | `gpu` | GPU power draw |
| `nv_gpu_clock_mhz` | gauge | `gpu`, `type` | GPU clock speed (graphics, memory) |
| `nv_gpu_memory_total_bytes` | gauge | `gpu` | GPU memory total (omitted on unified-memory GB10) |
| `nv_gpu_memory_used_bytes` | gauge | `gpu` | GPU memory used (omitted on unified-memory GB10) |
| `nv_gpu_fan_speed_percent` | gauge | `gpu` | Fan speed |
| `nv_gpu_encoder_utilization_percent` | gauge | `gpu` | Hardware encoder utilization |
| `nv_gpu_decoder_utilization_percent` | gauge | `gpu` | Hardware decoder utilization |
| `nv_gpu_performance_state` | gauge | `gpu` | P-state (0 = P0 / max clocks) |
| `nv_gpu_clocks_event_reasons` | gauge | `gpu` | Live throttle-reason bitmask (NVML/DCGM format) |
| `nv_gpu_throttle_duration_seconds_total` | counter | `gpu`, `reason` | Cumulative time clocks were held down per cause (`sw_power_cap`, `sw_thermal_slowdown`, `sync_boost`, `board_limit`, `low_utilization`, `reliability`) — `rate()` = throttle duty cycle |
| `nv_gpu_energy_millijoules_total` | counter | `gpu` | Cumulative GPU energy (`rate()/1000` = average watts) |
| `nv_gpu_pcie_replay_total` | counter | `gpu` | PCIe TLP replays (link-health counter) |
| `nv_rdma_info` | gauge | `device`, `port`, `state`, `rate` | RDMA port state and link rate |
| `nv_rdma_xmit_bytes_total`, `nv_rdma_recv_bytes_total` | counter | `device`, `port` | RDMA port traffic |
| `nv_rdma_xmit_packets_total`, `nv_rdma_recv_packets_total` | counter | `device`, `port` | RDMA port packet counts |
| `nv_rdma_errors_total` | counter | `device`, `port` | Sum of RDMA error counters |

GPU metrics gated on NVML support (`uuid`, `performance_state`, `clocks_event_reasons`, `throttle_duration`, `energy`, `pcie_replay`) are omitted silently where the driver/SoC does not expose them. On a DGX Spark (GB10) everything above emits except `gpu_memory_*` (unified memory) and `fan_speed` (no sensor).

### Prometheus scrape config

```yaml
scrape_configs:
  - job_name: 'nv-monitor'
    authorization:
      credentials: 'my-secret-token'
    static_configs:
      - targets: ['dgx-spark:9101']
```

No new dependencies are required — the exporter uses POSIX sockets and adds ~256 KB of memory overhead.

### Security

The exporter supports optional Bearer token authentication:

```bash
./nv-monitor -p 9101 -t my-secret-token           # token via CLI flag
NV_MONITOR_TOKEN=my-secret-token ./nv-monitor -p 9101  # token via env var (preferred)
```

The env var is preferred over `-t` since CLI arguments are visible in `ps` output. Without `-t` or `NV_MONITOR_TOKEN`, no auth is required (backwards compatible).

**Design rationale:** nv-monitor is a lightweight, single-purpose endpoint — it intentionally does not implement TLS. For transport security, layer it with the tools you already have:

- **Tailscale** — zero-config encrypted mesh, just run nv-monitor on the tailnet
- **SSH tunnel** — `ssh -L 9101:localhost:9101 dgx-spark`
- **Reverse proxy** — nginx/caddy with TLS termination
- **Service mesh** — Istio, Linkerd, etc.

This keeps the binary small, dependency-free, and composable with existing infrastructure.

## Synthetic Load Testing

A companion tool `demo-load` generates sinusoidal CPU and GPU loads for testing and multi-node validation — no bulky benchmarking tools required. See [DEMO-LOAD.md](DEMO-LOAD.md) for details.

```bash
make demo-load
./demo-load --gpu          # CPU + GPU sinusoidal load
```

## Requirements

- Linux (reads from `/proc` and `/sys`)
- NVIDIA drivers with NVML (for GPU monitoring — CPU/memory work without it)

### Platform support

| Platform | Status |
|----------|--------|
| DGX Spark (aarch64, Grace + GB10) | Primary target — full support including unified memory, HugePages, big.LITTLE core labels |
| GB200 NVL (aarch64, up to 208 GPUs) | Supported — dynamic allocation scales to any CPU/GPU count |
| Dell PowerEdge XE9680 (x86_64, 8x H100) | Tested — 112 cores, 8 GPUs with VRAM, multi-GPU Prometheus export |
| Jetson Orin (Nano / NX / AGX) | GPU via Tegra sysfs, A78AE core labels, legacy glibc binary available |
| Any Linux + NVIDIA GPU (x86_64) | Fully supported — CPU, memory, GPU Prometheus exporter |
| Linux without NVIDIA GPU | CPU and memory metrics only, no GPU metrics |
| RDMA / InfiniBand | Community-verified on real hardware — auto-detected via `/sys/class/infiniband/`, feedback welcome |

### A note on RDMA and cross-node bandwidth

nv-monitor captures per-port TX/RX throughput over QSFP/InfiniBand links — this is cross-node traffic by definition. In a Prometheus/Grafana setup you can visualise each node's fabric utilisation side by side and infer traffic flows.

For fabric-level visibility (topology, per-peer bandwidth, congestion maps, hop-by-hop latency), use your subnet manager or NVIDIA UFM alongside nv-monitor. Networking is a complex domain — nv-monitor focuses on endpoint metrics and leaves fabric management to dedicated tools.

## Contributors

- Prometheus metrics exporter by [Tim Messerschmidt (@SeraphimSerapis)](https://github.com/SeraphimSerapis)

## License

MIT
