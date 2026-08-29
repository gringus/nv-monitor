/*
 * nv-monitor - Prometheus exporter for NVIDIA DGX Spark (Grace + GB10)
 *
 * Headless exporter: CPU, memory, GPU, NIC, RDMA and thermal-zone metrics
 * on an HTTP endpoint (-p PORT).
 *
 * Build: gcc -O2 -o nv-monitor nv-monitor.c -ldl -lpthread
 */

#ifndef VERSION
#define VERSION "dev"
#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <dirent.h>
#include <signal.h>
#include <time.h>
#include <locale.h>
#include <sys/sysinfo.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <mntent.h>
#include <getopt.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>

/* ── NVML types (loaded dynamically) ────────────────────────────────── */

typedef void *nvmlDevice_t;
typedef int   nvmlReturn_t;

typedef struct {
    unsigned int gpu;
    unsigned int memory;
} nvmlUtilization_t;

typedef struct {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
} nvmlMemory_t;

#define NVML_SUCCESS 0
#define NVML_ERROR_NOT_SUPPORTED 3
#define NVML_TEMPERATURE_GPU 0
#define NVML_CLOCK_GRAPHICS 0
#define NVML_CLOCK_MEM 2
#define NVML_CLOCK_SM 1

/* NVML function pointers */
static nvmlReturn_t (*pNvmlInit)(void);
static nvmlReturn_t (*pNvmlShutdown)(void);
static nvmlReturn_t (*pNvmlDeviceGetCount)(unsigned int *);
static nvmlReturn_t (*pNvmlDeviceGetHandleByIndex)(unsigned int, nvmlDevice_t *);
static nvmlReturn_t (*pNvmlDeviceGetName)(nvmlDevice_t, char *, unsigned int);
static nvmlReturn_t (*pNvmlDeviceGetUtilizationRates)(nvmlDevice_t, nvmlUtilization_t *);
static nvmlReturn_t (*pNvmlDeviceGetMemoryInfo)(nvmlDevice_t, nvmlMemory_t *);
static nvmlReturn_t (*pNvmlDeviceGetTemperature)(nvmlDevice_t, int, unsigned int *);
static nvmlReturn_t (*pNvmlDeviceGetPowerUsage)(nvmlDevice_t, unsigned int *);
static nvmlReturn_t (*pNvmlDeviceGetClockInfo)(nvmlDevice_t, int, unsigned int *);
static nvmlReturn_t (*pNvmlDeviceGetFanSpeed)(nvmlDevice_t, unsigned int *);
static nvmlReturn_t (*pNvmlDeviceGetEncoderUtilization)(nvmlDevice_t, unsigned int *, unsigned int *);
static nvmlReturn_t (*pNvmlDeviceGetDecoderUtilization)(nvmlDevice_t, unsigned int *, unsigned int *);

static void *nvml_handle = NULL;
static int   nvml_ok = 0;
static unsigned int gpu_count = 0;      /* number of GPUs detected */
static int   use_tegra_gpu = 0;         /* prefer Tegra sysfs over NVML for GPU metrics */

/* ── Constants ──────────────────────────────────────────────────────── */

#define REFRESH_MS    1000
#define MAX_THERMAL_ZONES 20
#define THERMAL_BASE      "/sys/class/thermal" /* test_thermal.c overrides this */

/* ── CPU state (dynamically allocated at startup) ──────────────────── */

typedef struct {
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
} CpuTick;

static int       num_cpus = 0;
static int       max_cpus = 0;       /* allocated size */
static CpuTick  *prev_ticks = NULL;  /* [max_cpus + 1] — index 0 = aggregate */
static CpuTick  *cur_ticks = NULL;   /* [max_cpus + 1] — current frame */
static double   *cpu_pct = NULL;     /* [max_cpus + 1] */
static unsigned int *cpu_part = NULL; /* [max_cpus] */
static int      *cpu_freq_mhz = NULL; /* [max_cpus + 1] — per-core frequency */

/* ── Globals ────────────────────────────────────────────────────────── */

static volatile sig_atomic_t g_quit = 0;
static int delay_ms = REFRESH_MS;

/* Command-line options */
static int   prom_port = 0;  /* Prometheus metrics port (0 = not set) */
static const char *prom_token = NULL; /* Bearer token for /metrics auth */

/* ── Signal handler ─────────────────────────────────────────────────── */

static void on_signal(int sig) {
    (void)sig;
    g_quit = 1;
}

/* ── NVML loading ───────────────────────────────────────────────────── */

static int load_nvml(void) {
    const char *paths[] = {
        "libnvidia-ml.so.1",
        "libnvidia-ml.so",
        "/usr/lib/aarch64-linux-gnu/libnvidia-ml.so.1",
        "/usr/lib/x86_64-linux-gnu/libnvidia-ml.so.1",
        NULL
    };

    for (int i = 0; paths[i]; i++) {
        nvml_handle = dlopen(paths[i], RTLD_LAZY);
        if (nvml_handle) break;
    }
    if (!nvml_handle) return -1;

    /* Try versioned symbol first, then base name */
    #define LOAD(ptr, ...) do { \
        const char *_names[] = { __VA_ARGS__, NULL }; \
        for (int _i = 0; _names[_i]; _i++) { \
            *(void **)(&ptr) = dlsym(nvml_handle, _names[_i]); \
            if (ptr) break; \
        } \
    } while(0)

    LOAD(pNvmlInit,                               "nvmlInit_v2", "nvmlInit");
    LOAD(pNvmlShutdown,                           "nvmlShutdown");
    LOAD(pNvmlDeviceGetCount,                     "nvmlDeviceGetCount_v2", "nvmlDeviceGetCount");
    LOAD(pNvmlDeviceGetHandleByIndex,             "nvmlDeviceGetHandleByIndex_v2", "nvmlDeviceGetHandleByIndex");
    LOAD(pNvmlDeviceGetName,                      "nvmlDeviceGetName");
    LOAD(pNvmlDeviceGetUtilizationRates,          "nvmlDeviceGetUtilizationRates");
    LOAD(pNvmlDeviceGetMemoryInfo,                "nvmlDeviceGetMemoryInfo");
    LOAD(pNvmlDeviceGetTemperature,               "nvmlDeviceGetTemperature");
    LOAD(pNvmlDeviceGetPowerUsage,                "nvmlDeviceGetPowerUsage");
    LOAD(pNvmlDeviceGetClockInfo,                 "nvmlDeviceGetClockInfo");
    LOAD(pNvmlDeviceGetFanSpeed,                  "nvmlDeviceGetFanSpeed");
    LOAD(pNvmlDeviceGetEncoderUtilization,        "nvmlDeviceGetEncoderUtilization");
    LOAD(pNvmlDeviceGetDecoderUtilization,        "nvmlDeviceGetDecoderUtilization");
    #undef LOAD

    if (!pNvmlInit) return -1;
    if (pNvmlInit() != NVML_SUCCESS) return -1;

    return 0;
}

/* ── CPU core type identification ───────────────────────────────────── */

static void read_cpu_part_ids(void) {
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return;
    char line[256];
    int cur_cpu = -1;
    while (fgets(line, sizeof(line), f)) {
        int n;
        if (sscanf(line, "processor : %d", &n) == 1) {
            cur_cpu = n;
        } else if (cur_cpu >= 0 && cur_cpu < max_cpus) {
            unsigned int part;
            if (sscanf(line, "CPU part : %x", &part) == 1)
                cpu_part[cur_cpu] = part;
        }
    }
    fclose(f);
}

static const char *cpu_part_label(int cpu_idx) {
    switch (cpu_part[cpu_idx]) {
    /* Grace (DGX Spark / DGX 300) */
    case 0xd85: return "X925";
    case 0xd87: return "X725";
    /* Jetson Orin (Nano / NX / AGX) */
    case 0xd42: return "A78A";  /* Cortex-A78AE */
    /* Other common ARM cores */
    case 0xd44: return "X4";
    case 0xd43: return "A720";
    case 0xd46: return "A725";
    case 0xd41: return "A78";
    case 0xd40: return "V2";
    case 0xd0b: return "A76";
    case 0xd0a: return "A75";
    case 0xd07: return "A57";   /* Jetson TX1/TX2 */
    case 0xd03: return "A53";   /* Jetson Nano (original) */
    default:    return "";
    }
}

/* ── CPU sampling ───────────────────────────────────────────────────── */

static void read_cpu_ticks(CpuTick ticks[], int *n_cpus) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return;

    char line[512];
    int idx = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "cpu", 3) != 0) continue;
        CpuTick t = {0};
        if (line[3] == ' ') {
            /* aggregate */
            sscanf(line + 4, "%llu %llu %llu %llu %llu %llu %llu %llu",
                   &t.user, &t.nice, &t.system, &t.idle,
                   &t.iowait, &t.irq, &t.softirq, &t.steal);
            ticks[0] = t;
        } else {
            int cpunum;
            sscanf(line + 3, "%d", &cpunum);
            sscanf(strchr(line + 3, ' ') + 1, "%llu %llu %llu %llu %llu %llu %llu %llu",
                   &t.user, &t.nice, &t.system, &t.idle,
                   &t.iowait, &t.irq, &t.softirq, &t.steal);
            if (cpunum + 1 < max_cpus) {
                ticks[cpunum + 1] = t;
                idx = cpunum + 1;
            }
        }
    }
    *n_cpus = idx;
    fclose(f);
}

static void compute_cpu_usage(void) {
    memset(cur_ticks, 0, (max_cpus + 1) * sizeof(CpuTick));
    int n = 0;
    read_cpu_ticks(cur_ticks, &n);
    if (n > max_cpus) n = max_cpus;
    num_cpus = n;

    for (int i = 0; i <= n; i++) {
        unsigned long long prev_idle  = prev_ticks[i].idle + prev_ticks[i].iowait;
        unsigned long long cur_idle   = cur_ticks[i].idle + cur_ticks[i].iowait;
        unsigned long long prev_total = prev_ticks[i].user + prev_ticks[i].nice +
                                        prev_ticks[i].system + prev_ticks[i].idle +
                                        prev_ticks[i].iowait + prev_ticks[i].irq +
                                        prev_ticks[i].softirq + prev_ticks[i].steal;
        unsigned long long cur_total  = cur_ticks[i].user + cur_ticks[i].nice +
                                        cur_ticks[i].system + cur_ticks[i].idle +
                                        cur_ticks[i].iowait + cur_ticks[i].irq +
                                        cur_ticks[i].softirq + cur_ticks[i].steal;
        unsigned long long totald = cur_total - prev_total;
        unsigned long long idled  = cur_idle - prev_idle;
        if (totald == 0)
            cpu_pct[i] = 0.0;
        else
            cpu_pct[i] = (double)(totald - idled) / (double)totald * 100.0;
    }

    memcpy(prev_ticks, cur_ticks, (max_cpus + 1) * sizeof(CpuTick));
}

/* ── Memory info ────────────────────────────────────────────────────── */

typedef struct {
    /* Raw values from /proc/meminfo */
    unsigned long long total_kb;
    unsigned long long free_kb;
    unsigned long long avail_kb;
    unsigned long long buffers_kb;
    unsigned long long cached_kb;
    unsigned long long swap_total_kb;
    unsigned long long swap_free_kb;
    /* Derived values */
    unsigned long long app_kb;      /* actual application memory */
    unsigned long long bufcache_kb; /* buffers + cached */
    unsigned long long swap_used_kb;
} MemInfo;

/* Compute derived fields from raw /proc/meminfo values */
static void meminfo_calc(MemInfo *m) {
    m->bufcache_kb = m->buffers_kb + m->cached_kb;
    m->app_kb = (m->total_kb > m->free_kb + m->bufcache_kb)
              ? m->total_kb - m->free_kb - m->bufcache_kb : 0;
    m->swap_used_kb = (m->swap_total_kb > m->swap_free_kb)
                    ? m->swap_total_kb - m->swap_free_kb : 0;
}

static void read_meminfo(MemInfo *m) {
    memset(m, 0, sizeof(*m));
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;

    long long huge_total = -1, huge_free = -1, huge_size = -1;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemTotal: %llu kB", &m->total_kb) == 1) continue;
        if (sscanf(line, "MemFree: %llu kB", &m->free_kb) == 1) continue;
        if (sscanf(line, "MemAvailable: %llu kB", &m->avail_kb) == 1) continue;
        if (sscanf(line, "Buffers: %llu kB", &m->buffers_kb) == 1) continue;
        if (sscanf(line, "Cached: %llu kB", &m->cached_kb) == 1) continue;
        if (sscanf(line, "SwapTotal: %llu kB", &m->swap_total_kb) == 1) continue;
        if (sscanf(line, "SwapFree: %llu kB", &m->swap_free_kb) == 1) continue;
        if (sscanf(line, "HugePages_Total: %lld", &huge_total) == 1) continue;
        if (sscanf(line, "HugePages_Free: %lld", &huge_free) == 1) continue;
        if (sscanf(line, "Hugepagesize: %lld kB", &huge_size) == 1) continue;
    }
    fclose(f);

    /* DGX Spark: when HugePages are active, MemAvailable is inaccurate.
     * Use HugePages_Free * Hugepagesize instead, and report swap as 0
     * since hugetlbfs pages are not swappable.
     * See: docs.nvidia.com/dgx/dgx-spark/known-issues.html */
    if (huge_total > 0 && huge_free >= 0 && huge_size > 0) {
        m->avail_kb = (unsigned long long)(huge_free * huge_size);
        m->swap_free_kb = m->swap_total_kb; /* effective 0 swap used */
    }

    meminfo_calc(m);
}

/* ── CPU frequency ──────────────────────────────────────────────────── */

static void read_cpu_freqs(void) {
    /* Aggregate frequency (cpu0) for backward compatibility */
    FILE *f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
    if (f) {
        int khz = 0;
        (void)!fscanf(f, "%d", &khz);
        fclose(f);
        cpu_freq_mhz[0] = khz / 1000;
    }
    /* Per-core frequencies */
    for (int i = 1; i <= num_cpus; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i - 1);
        f = fopen(path, "r");
        if (f) {
            int khz = 0;
            (void)!fscanf(f, "%d", &khz);
            fclose(f);
            cpu_freq_mhz[i] = khz / 1000;
        }
    }
}

/* ── Tegra GPU sysfs fallback (Jetson Orin / Nano / NX / AGX) ──────── */

static int tegra_gpu_available = 0;
static char tegra_gpu_load_path[256] = "";
static int tegra_gpu_therm_zone = -1; /* thermal zone index for GPU-therm */

static void detect_tegra_gpu(void) {
    /* Try known Tegra GPU load paths */
    const char *gpu_paths[] = {
        "/sys/devices/gpu.0/load",
        "/sys/devices/platform/bus@0/17000000.gpu/load",
        "/sys/devices/platform/17000000.gpu/load",
        NULL
    };
    for (int i = 0; gpu_paths[i]; i++) {
        FILE *f = fopen(gpu_paths[i], "r");
        if (f) {
            tegra_gpu_available = 1;
            snprintf(tegra_gpu_load_path, sizeof(tegra_gpu_load_path), "%s", gpu_paths[i]);
            fclose(f);
            break;
        }
    }

    /* Find GPU thermal zone */
    for (int i = 0; i < MAX_THERMAL_ZONES; i++) {
        char path[128], type[64] = "";
        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/type", i);
        FILE *f = fopen(path, "r");
        if (!f) break;
        if (fgets(type, sizeof(type), f)) {
            type[strcspn(type, "\n\r")] = '\0';
            if (strcasecmp(type, "GPU-therm") == 0 ||
                strcasecmp(type, "gpu-thermal") == 0) {
                tegra_gpu_therm_zone = i;
                fclose(f);
                break;
            }
        }
        fclose(f);
    }
}

static int read_tegra_gpu_util(void) {
    FILE *f = fopen(tegra_gpu_load_path, "r");
    if (!f) return -1;
    int load = 0;
    (void)!fscanf(f, "%d", &load);
    fclose(f);
    return load / 10; /* scale is 0-1000 -> 0-100% */
}

static int read_tegra_gpu_temp(void) {
    if (tegra_gpu_therm_zone < 0) return -1;
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", tegra_gpu_therm_zone);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int t = 0;
    (void)!fscanf(f, "%d", &t);
    fclose(f);
    return t / 1000;
}

/* ── NIC ASIC temperature (ConnectX / mlx5) ─────────────────────────── */

#define MAX_NIC_SENSORS 8

static char nic_sensor_paths[MAX_NIC_SENSORS][128];
static int  nic_sensor_count = 0;

static void detect_nic_asic_sensors(void) {
    DIR *dir = opendir("/sys/class/hwmon");
    if (!dir) return;

    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) && nic_sensor_count < MAX_NIC_SENSORS) {
        if (strncmp(ent->d_name, "hwmon", 5) != 0) continue;

        char name[64] = "";
        snprintf(name, sizeof(name), "/sys/class/hwmon/%s/name", ent->d_name);
        FILE *f = fopen(name, "r");
        if (f) {
            if (fgets(name, sizeof(name), f))
                name[strcspn(name, "\n\r")] = '\0';
            fclose(f);
        }
        if (strcmp(name, "mlx5") != 0) continue;

        /* Verify temp1_input exists */
        char path[192];
        snprintf(path, sizeof(path), "/sys/class/hwmon/%s/temp1_input", ent->d_name);
        f = fopen(path, "r");
        if (!f) continue;
        fclose(f);
        snprintf(nic_sensor_paths[nic_sensor_count],
                 sizeof(nic_sensor_paths[0]), "%s", path);
        nic_sensor_count++;
    }
    closedir(dir);
}

/* Return hottest ASIC temperature in deg C, 0 if no sensor found */
static int read_nic_asic_temp(void) {
    int max_temp = 0;
    for (int i = 0; i < nic_sensor_count; i++) {
        FILE *f = fopen(nic_sensor_paths[i], "r");
        if (!f) continue;
        int millideg = 0;
        if (fscanf(f, "%d", &millideg) == 1 && millideg / 1000 > max_temp)
            max_temp = millideg / 1000;
        fclose(f);
    }
    return max_temp;
}

/* ── Load average ───────────────────────────────────────────────────── */

static void get_loadavg(double *l1, double *l5, double *l15) {
    *l1 = *l5 = *l15 = 0.0;
    FILE *f = fopen("/proc/loadavg", "r");
    if (f) { (void)!fscanf(f, "%lf %lf %lf", l1, l5, l15); fclose(f); }
}

/* ── Aggregate network throughput ───────────────────────────────────── */

typedef struct {
    unsigned long long rx_bytes;
    unsigned long long tx_bytes;
    double rx_bytes_sec;
    double tx_bytes_sec;
    int valid;
} NetTotals;

static NetTotals net_totals = {0};
static unsigned long long net_prev_rx = 0;
static unsigned long long net_prev_tx = 0;
static struct timespec    net_prev_time;
static int                net_prev_valid = 0;
static double             net_scale_bytes_sec = 1024.0 * 1024.0; /* auto-scaling floor: 1 MiB/s */

static void read_net_totals(void) {
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) {
        net_totals.valid = 0;
        return;
    }

    unsigned long long rx_total = 0;
    unsigned long long tx_total = 0;
    char line[512];
    int line_no = 0;
    while (fgets(line, sizeof(line), f)) {
        line_no++;
        if (line_no <= 2) continue;

        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';

        char ifname[64];
        snprintf(ifname, sizeof(ifname), "%s", line);
        char *name = ifname;
        while (*name == ' ') name++;
        if (strcmp(name, "lo") == 0) continue;

        unsigned long long rx_bytes = 0, tx_bytes = 0;
        unsigned long long discard[14];
        if (sscanf(colon + 1,
                   " %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &rx_bytes,
                   &discard[0], &discard[1], &discard[2], &discard[3], &discard[4], &discard[5], &discard[6],
                   &tx_bytes,
                   &discard[7], &discard[8], &discard[9], &discard[10], &discard[11], &discard[12], &discard[13]) == 16) {
            rx_total += rx_bytes;
            tx_total += tx_bytes;
        }
    }
    fclose(f);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double dt = 0.0;
    if (net_prev_valid) {
        dt = (now.tv_sec - net_prev_time.tv_sec) +
             (now.tv_nsec - net_prev_time.tv_nsec) / 1e9;
        if (dt <= 0) dt = 1.0;
    }

    net_totals.rx_bytes = rx_total;
    net_totals.tx_bytes = tx_total;
    if (net_prev_valid) {
        if (rx_total >= net_prev_rx)
            net_totals.rx_bytes_sec = (double)(rx_total - net_prev_rx) / dt;
        else
            net_totals.rx_bytes_sec = 0;  /* counter wrapped, skip frame */
        if (tx_total >= net_prev_tx)
            net_totals.tx_bytes_sec = (double)(tx_total - net_prev_tx) / dt;
        else
            net_totals.tx_bytes_sec = 0;  /* counter wrapped, skip frame */
    } else {
        net_totals.rx_bytes_sec = 0;
        net_totals.tx_bytes_sec = 0;
    }
    net_totals.valid = 1;
    double total_bytes_sec = net_totals.rx_bytes_sec + net_totals.tx_bytes_sec;
    double decayed_scale = net_scale_bytes_sec * 0.95;
    if (decayed_scale < 1024.0 * 1024.0) decayed_scale = 1024.0 * 1024.0;
    net_scale_bytes_sec = total_bytes_sec > decayed_scale ? total_bytes_sec : decayed_scale;
    net_prev_rx = rx_total;
    net_prev_tx = tx_total;
    net_prev_time = now;
    net_prev_valid = 1;
}

/* ── RDMA types (used by CSV logging and Prometheus) ───────────────── */

#define MAX_RDMA_PORTS 16

typedef struct {
    char device[64];
    int  port;
    char state[32];
    char rate[32];
    unsigned long long xmit_bytes;
    unsigned long long recv_bytes;
    unsigned long long xmit_pkts;
    unsigned long long recv_pkts;
    unsigned long long errors;
    double xmit_bytes_sec;
    double recv_bytes_sec;
} RdmaPort;

static RdmaPort rdma_ports[MAX_RDMA_PORTS];
static int       rdma_count = 0;
static int       rdma_available = 0;

/* ── RDMA / InfiniBand monitoring ───────────────────────────────────── */

static unsigned long long rdma_prev_xmit[MAX_RDMA_PORTS];
static unsigned long long rdma_prev_recv[MAX_RDMA_PORTS];
static struct timespec    rdma_prev_time;
static int                rdma_prev_valid = 0;

static unsigned long long read_sysfs_ull(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    unsigned long long val = 0;
    (void)!fscanf(f, "%llu", &val);
    fclose(f);
    return val;
}

static void read_sysfs_str(const char *path, char *buf, int len) {
    FILE *f = fopen(path, "r");
    if (!f) { buf[0] = '\0'; return; }
    if (!fgets(buf, len, f)) buf[0] = '\0';
    fclose(f);
    buf[strcspn(buf, "\n\r")] = '\0';
}

static void read_rdma_ports(void) {
    DIR *ib_dir = opendir("/sys/class/infiniband");
    if (!ib_dir) { rdma_available = 0; rdma_count = 0; return; }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double dt = 0;
    if (rdma_prev_valid) {
        dt = (now.tv_sec - rdma_prev_time.tv_sec) +
             (now.tv_nsec - rdma_prev_time.tv_nsec) / 1e9;
        if (dt <= 0) dt = 1;
    }

    rdma_available = 1;
    int idx = 0;
    struct dirent *dev_ent;
    while ((dev_ent = readdir(ib_dir)) && idx < MAX_RDMA_PORTS) {
        if (dev_ent->d_name[0] == '.') continue;

        /* Scan ports (typically 1-2) */
        for (int p = 1; p <= 2 && idx < MAX_RDMA_PORTS; p++) {
            char path[256];
            snprintf(path, sizeof(path), "/sys/class/infiniband/%s/ports/%d/state", dev_ent->d_name, p);
            FILE *test = fopen(path, "r");
            if (!test) continue;
            fclose(test);

            RdmaPort *r = &rdma_ports[idx];
            snprintf(r->device, sizeof(r->device), "%s", dev_ent->d_name);
            r->port = p;

            read_sysfs_str(path, r->state, sizeof(r->state));
            /* Strip numeric prefix like "4: ACTIVE" -> "ACTIVE" */
            char *colon = strchr(r->state, ':');
            if (colon) {
                const char *s = colon + 1;
                while (*s == ' ') s++;
                memmove(r->state, s, strlen(s) + 1);
            }

            snprintf(path, sizeof(path), "/sys/class/infiniband/%s/ports/%d/rate", dev_ent->d_name, p);
            read_sysfs_str(path, r->rate, sizeof(r->rate));

            /* Counters — RDMA counters are in units of 4 bytes (32-bit words) for data */
            snprintf(path, sizeof(path), "/sys/class/infiniband/%s/ports/%d/counters/port_xmit_data", dev_ent->d_name, p);
            r->xmit_bytes = read_sysfs_ull(path) * 4;
            snprintf(path, sizeof(path), "/sys/class/infiniband/%s/ports/%d/counters/port_rcv_data", dev_ent->d_name, p);
            r->recv_bytes = read_sysfs_ull(path) * 4;
            snprintf(path, sizeof(path), "/sys/class/infiniband/%s/ports/%d/counters/port_xmit_packets", dev_ent->d_name, p);
            r->xmit_pkts = read_sysfs_ull(path);
            snprintf(path, sizeof(path), "/sys/class/infiniband/%s/ports/%d/counters/port_rcv_packets", dev_ent->d_name, p);
            r->recv_pkts = read_sysfs_ull(path);

            /* Sum error counters */
            r->errors = 0;
            const char *err_counters[] = {
                "symbol_error_counter", "port_rcv_errors",
                "port_rcv_constraint_errors", "port_xmit_constraint_errors",
                "link_error_recovery_counter", "link_downed_counter",
                NULL
            };
            for (int e = 0; err_counters[e]; e++) {
                snprintf(path, sizeof(path), "/sys/class/infiniband/%s/ports/%d/counters/%s",
                         dev_ent->d_name, p, err_counters[e]);
                r->errors += read_sysfs_ull(path);
            }

            /* Rate calculation */
            if (rdma_prev_valid && idx < rdma_count) {
                if (r->xmit_bytes >= rdma_prev_xmit[idx])
                    r->xmit_bytes_sec = (double)(r->xmit_bytes - rdma_prev_xmit[idx]) / dt;
                else
                    r->xmit_bytes_sec = 0;  /* counter wrapped, skip frame */
                if (r->recv_bytes >= rdma_prev_recv[idx])
                    r->recv_bytes_sec = (double)(r->recv_bytes - rdma_prev_recv[idx]) / dt;
                else
                    r->recv_bytes_sec = 0;  /* counter wrapped, skip frame */
            } else {
                r->xmit_bytes_sec = 0;
                r->recv_bytes_sec = 0;
            }

            rdma_prev_xmit[idx] = r->xmit_bytes;
            rdma_prev_recv[idx] = r->recv_bytes;
            idx++;
        }
    }
    closedir(ib_dir);
    rdma_count = idx;
    rdma_prev_time = now;
    rdma_prev_valid = 1;
}

/* ── Prometheus metrics exporter ────────────────────────────────────── */

static int   prom_sock = -1;
static pthread_t prom_thread;

#define PROM_BYTES_PER_GPU 512  /* estimated Prometheus output per GPU */
#define PROM_BASE_SIZE 8192     /* base buffer for CPU/memory/system metrics */

typedef struct {
    int      valid;
    char     name[96];
    unsigned int util_gpu;
    unsigned int temp;
    unsigned int power_mw;
    int      has_power;
    unsigned int clk_gfx, clk_mem;
    unsigned long long mem_total, mem_used;
    int      has_mem;
    unsigned int fan;
    int      has_fan;
    unsigned int enc, dec;
    int      has_enc, has_dec;
} PromGpu;

static int      prom_buf_size = 0;
static char    *prom_body = NULL;
static PromGpu *prom_gpus = NULL;

/* Read all thermal zones. Returns highest valid zone index + 1 (0 = none);
 * caller skips entries with empty type. temps[] in degrees Celsius. */
static int read_thermal_zones(double temps[], char types[][64]) {
    int n = 0;
    for (int i = 0; i < MAX_THERMAL_ZONES; i++) {
        char path[128];
        snprintf(path, sizeof(path), THERMAL_BASE "/thermal_zone%d/type", i);
        read_sysfs_str(path, types[i], 64);
        if (!types[i][0]) continue;
        snprintf(path, sizeof(path), THERMAL_BASE "/thermal_zone%d/temp", i);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        int millideg = 0;
        if (fscanf(f, "%d", &millideg) == 1) {
            temps[i] = millideg / 1000.0;
            n = i + 1;
        }
        fclose(f);
    }
    return n;
}

/* Format all metrics into buf. Returns bytes written. */
static int format_metrics(char *buf, int buflen) {
    int off = 0;

    #define PM(...) do { \
        int _n = snprintf(buf + off, (size_t)(buflen - off), __VA_ARGS__); \
        if (_n > 0) { \
            if (_n >= buflen - off) { off = buflen - 1; goto pm_done; } \
            off += _n; \
        } \
    } while(0)

    /* Build info */
    PM("# HELP nv_build_info nv-monitor version\n"
       "# TYPE nv_build_info gauge\n"
       "nv_build_info{version=\"%s\"} 1\n", VERSION);

    /* Uptime */
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        PM("# HELP nv_uptime_seconds System uptime\n"
           "# TYPE nv_uptime_seconds gauge\n"
           "nv_uptime_seconds %ld\n", si.uptime);
    }

    /* Load average */
    double l1 = 0, l5 = 0, l15 = 0;
    get_loadavg(&l1, &l5, &l15);
    PM("# HELP nv_load_average System load average\n"
       "# TYPE nv_load_average gauge\n"
       "nv_load_average{interval=\"1m\"} %.2f\n"
       "nv_load_average{interval=\"5m\"} %.2f\n"
       "nv_load_average{interval=\"15m\"} %.2f\n", l1, l5, l15);

    /* CPU usage */
    PM("# HELP nv_cpu_usage_percent CPU utilization\n"
       "# TYPE nv_cpu_usage_percent gauge\n");
    for (int i = 1; i <= num_cpus; i++) {
        const char *lbl = cpu_part_label(i - 1);
        if (lbl[0])
            PM("nv_cpu_usage_percent{cpu=\"%d\",type=\"%s\"} %.1f\n",
               i - 1, lbl, cpu_pct[i]);
        else
            PM("nv_cpu_usage_percent{cpu=\"%d\"} %.1f\n",
               i - 1, cpu_pct[i]);
    }

    /* Per-thermal-zone temperatures (zone index + kernel zone type) */
    {
        double tz_temp[MAX_THERMAL_ZONES];
        char tz_type[MAX_THERMAL_ZONES][64];
        int tz_max = read_thermal_zones(tz_temp, tz_type);
        if (tz_max > 0) {
            PM("# HELP nv_thermal_zone_temperature_celsius Thermal zone temperature\n"
               "# TYPE nv_thermal_zone_temperature_celsius gauge\n");
            for (int i = 0; i < tz_max; i++)
                if (tz_type[i][0])
                    PM("nv_thermal_zone_temperature_celsius{zone=\"%d\",type=\"%s\"} %.1f\n",
                       i, tz_type[i], tz_temp[i]);
        }
    }

    /* CPU frequency */
    read_cpu_freqs();
    PM("# HELP nv_cpu_frequency_mhz CPU frequency\n"
       "# TYPE nv_cpu_frequency_mhz gauge\n");
    for (int i = 1; i <= num_cpus; i++) {
        const char *lbl = cpu_part_label(i - 1);
        if (lbl[0])
            PM("nv_cpu_frequency_mhz{cpu=\"%d\",type=\"%s\"} %d\n",
               i - 1, lbl, cpu_freq_mhz[i]);
        else
            PM("nv_cpu_frequency_mhz{cpu=\"%d\"} %d\n",
               i - 1, cpu_freq_mhz[i]);
    }

    /* Memory */
    MemInfo mi;
    read_meminfo(&mi);
    PM("# HELP nv_memory_total_bytes Total system memory\n"
       "# TYPE nv_memory_total_bytes gauge\n"
       "nv_memory_total_bytes %llu\n"
       "# HELP nv_memory_used_bytes Application memory used\n"
       "# TYPE nv_memory_used_bytes gauge\n"
       "nv_memory_used_bytes %llu\n"
       "# HELP nv_memory_bufcache_bytes Buffer and cache memory\n"
       "# TYPE nv_memory_bufcache_bytes gauge\n"
       "nv_memory_bufcache_bytes %llu\n",
       mi.total_kb * 1024ULL, mi.app_kb * 1024ULL, mi.bufcache_kb * 1024ULL);

    if (mi.swap_total_kb > 0) {
        PM("# HELP nv_swap_total_bytes Total swap\n"
           "# TYPE nv_swap_total_bytes gauge\n"
           "nv_swap_total_bytes %llu\n"
           "# HELP nv_swap_used_bytes Swap used\n"
           "# TYPE nv_swap_used_bytes gauge\n"
           "nv_swap_used_bytes %llu\n",
           mi.swap_total_kb * 1024ULL, mi.swap_used_kb * 1024ULL);
    }

    if (net_totals.valid) {
        PM("# HELP nv_network_receive_bytes_total Total bytes received across all non-loopback interfaces\n"
           "# TYPE nv_network_receive_bytes_total counter\n"
           "nv_network_receive_bytes_total %llu\n"
           "# HELP nv_network_transmit_bytes_total Total bytes transmitted across all non-loopback interfaces\n"
           "# TYPE nv_network_transmit_bytes_total counter\n"
           "nv_network_transmit_bytes_total %llu\n"
           "# HELP nv_network_receive_bytes_per_second Aggregate receive throughput across all non-loopback interfaces\n"
           "# TYPE nv_network_receive_bytes_per_second gauge\n"
           "nv_network_receive_bytes_per_second %.0f\n"
           "# HELP nv_network_transmit_bytes_per_second Aggregate transmit throughput across all non-loopback interfaces\n"
           "# TYPE nv_network_transmit_bytes_per_second gauge\n"
           "nv_network_transmit_bytes_per_second %.0f\n",
           net_totals.rx_bytes, net_totals.tx_bytes,
           net_totals.rx_bytes_sec, net_totals.tx_bytes_sec);
    }

    int nic_temp = read_nic_asic_temp();
    if (nic_temp > 0) {
        PM("# HELP nv_nic_asic_temperature_celsius NIC ASIC temperature (mlx5/ConnectX, hottest sensor)\n"
           "# TYPE nv_nic_asic_temperature_celsius gauge\n"
           "nv_nic_asic_temperature_celsius %d\n", nic_temp);
    }

    /* Disk usage per real mountpoint (skip pseudo/virtual fs) */
    {
        struct {
            char mount[256];
            char fstype[32];
            char device[128];
            unsigned long long total;
            unsigned long long avail;
            unsigned long long used;
        } disks[64];
        int n_disks = 0;

        FILE *mf = setmntent("/proc/mounts", "r");
        if (mf) {
            struct mntent *me;
            while ((me = getmntent(mf)) != NULL && n_disks < 64) {
                /* Skip pseudo / virtual filesystems */
                if (strcmp(me->mnt_type, "tmpfs") == 0 ||
                    strcmp(me->mnt_type, "devtmpfs") == 0 ||
                    strcmp(me->mnt_type, "overlay") == 0 ||
                    strcmp(me->mnt_type, "squashfs") == 0 ||
                    strcmp(me->mnt_type, "proc") == 0 ||
                    strcmp(me->mnt_type, "sysfs") == 0 ||
                    strcmp(me->mnt_type, "cgroup") == 0 ||
                    strcmp(me->mnt_type, "cgroup2") == 0 ||
                    strcmp(me->mnt_type, "devpts") == 0 ||
                    strcmp(me->mnt_type, "mqueue") == 0 ||
                    strcmp(me->mnt_type, "hugetlbfs") == 0 ||
                    strcmp(me->mnt_type, "debugfs") == 0 ||
                    strcmp(me->mnt_type, "tracefs") == 0 ||
                    strcmp(me->mnt_type, "fusectl") == 0 ||
                    strcmp(me->mnt_type, "configfs") == 0 ||
                    strcmp(me->mnt_type, "pstore") == 0 ||
                    strcmp(me->mnt_type, "bpf") == 0 ||
                    strcmp(me->mnt_type, "autofs") == 0 ||
                    strcmp(me->mnt_type, "binfmt_misc") == 0 ||
                    strcmp(me->mnt_type, "rpc_pipefs") == 0 ||
                    strcmp(me->mnt_type, "nsfs") == 0 ||
                    strcmp(me->mnt_type, "securityfs") == 0 ||
                    strcmp(me->mnt_type, "efivarfs") == 0 ||
                    strncmp(me->mnt_fsname, "/dev/loop", 9) == 0)
                    continue;
                /* Only real device-backed mounts */
                if (me->mnt_fsname[0] != '/')
                    continue;

                struct statvfs sv;
                if (statvfs(me->mnt_dir, &sv) != 0)
                    continue;

                unsigned long long total = (unsigned long long)sv.f_blocks * sv.f_frsize;
                unsigned long long avail = (unsigned long long)sv.f_bavail * sv.f_frsize;
                if (total == 0)
                    continue;
                unsigned long long used = total - (unsigned long long)sv.f_bfree * sv.f_frsize;

                snprintf(disks[n_disks].mount, sizeof(disks[n_disks].mount), "%s", me->mnt_dir);
                snprintf(disks[n_disks].fstype, sizeof(disks[n_disks].fstype), "%s", me->mnt_type);
                snprintf(disks[n_disks].device, sizeof(disks[n_disks].device), "%s", me->mnt_fsname);
                disks[n_disks].total = total;
                disks[n_disks].avail = avail;
                disks[n_disks].used = used;
                n_disks++;
            }
            endmntent(mf);
        }

        if (n_disks > 0) {
            PM("# HELP nv_disk_total_bytes Filesystem total size\n"
               "# TYPE nv_disk_total_bytes gauge\n");
            for (int i = 0; i < n_disks; i++)
                PM("nv_disk_total_bytes{mountpoint=\"%s\",device=\"%s\",fstype=\"%s\"} %llu\n",
                   disks[i].mount, disks[i].device, disks[i].fstype, disks[i].total);

            PM("# HELP nv_disk_used_bytes Filesystem used bytes\n"
               "# TYPE nv_disk_used_bytes gauge\n");
            for (int i = 0; i < n_disks; i++)
                PM("nv_disk_used_bytes{mountpoint=\"%s\",device=\"%s\",fstype=\"%s\"} %llu\n",
                   disks[i].mount, disks[i].device, disks[i].fstype, disks[i].used);

            PM("# HELP nv_disk_avail_bytes Filesystem available bytes\n"
               "# TYPE nv_disk_avail_bytes gauge\n");
            for (int i = 0; i < n_disks; i++)
                PM("nv_disk_avail_bytes{mountpoint=\"%s\",device=\"%s\",fstype=\"%s\"} %llu\n",
                   disks[i].mount, disks[i].device, disks[i].fstype, disks[i].avail);
        }
    }

    /* GPU — collect data first, then format grouped by metric family */
    PromGpu *gpus = prom_gpus;
    int n_gpus = 0;
    if (gpus && gpu_count > 0)
        memset(gpus, 0, gpu_count * sizeof(PromGpu));

    if (nvml_ok) {
        unsigned int dev_count = 0;
        pNvmlDeviceGetCount(&dev_count);

        for (unsigned int d = 0; gpus && d < dev_count && d < gpu_count; d++) {
            if ((unsigned int)n_gpus >= gpu_count) break;
            PromGpu *g = &gpus[n_gpus];
            memset(g, 0, sizeof(*g));
            nvmlDevice_t dev;
            if (pNvmlDeviceGetHandleByIndex(d, &dev) != NVML_SUCCESS) continue;
            g->valid = 1;
            pNvmlDeviceGetName(dev, g->name, sizeof(g->name));

            nvmlUtilization_t util = {0};
            if (!use_tegra_gpu && pNvmlDeviceGetUtilizationRates)
                pNvmlDeviceGetUtilizationRates(dev, &util);
            if (use_tegra_gpu) {
                int tutil = read_tegra_gpu_util();
                if (tutil >= 0) util.gpu = (unsigned int)tutil;
            }
            g->util_gpu = util.gpu;

            if (!use_tegra_gpu && pNvmlDeviceGetTemperature)
                pNvmlDeviceGetTemperature(dev, NVML_TEMPERATURE_GPU, &g->temp);
            if (use_tegra_gpu && tegra_gpu_therm_zone >= 0) {
                int ttemp = read_tegra_gpu_temp();
                if (ttemp > 0) g->temp = (unsigned int)ttemp;
            }

            g->has_power = (pNvmlDeviceGetPowerUsage &&
                            pNvmlDeviceGetPowerUsage(dev, &g->power_mw) == NVML_SUCCESS);

            if (pNvmlDeviceGetClockInfo) {
                pNvmlDeviceGetClockInfo(dev, NVML_CLOCK_GRAPHICS, &g->clk_gfx);
                pNvmlDeviceGetClockInfo(dev, NVML_CLOCK_MEM, &g->clk_mem);
            }

            nvmlMemory_t mem = {0};
            g->has_mem = (pNvmlDeviceGetMemoryInfo &&
                          pNvmlDeviceGetMemoryInfo(dev, &mem) == NVML_SUCCESS &&
                          mem.total > 0);
            if (g->has_mem) { g->mem_total = mem.total; g->mem_used = mem.used; }

            unsigned int period;
            g->has_fan = (pNvmlDeviceGetFanSpeed &&
                          pNvmlDeviceGetFanSpeed(dev, &g->fan) == NVML_SUCCESS);
            g->has_enc = (pNvmlDeviceGetEncoderUtilization &&
                          pNvmlDeviceGetEncoderUtilization(dev, &g->enc, &period) == NVML_SUCCESS);
            g->has_dec = (pNvmlDeviceGetDecoderUtilization &&
                          pNvmlDeviceGetDecoderUtilization(dev, &g->dec, &period) == NVML_SUCCESS);
            n_gpus++;
        }
    }

    if (n_gpus > 0) {
        PM("# HELP nv_gpu_info GPU device information\n"
           "# TYPE nv_gpu_info gauge\n");
        for (int d = 0; d < n_gpus; d++)
            PM("nv_gpu_info{gpu=\"%d\",name=\"%s\"} 1\n", d, gpus[d].name);

        PM("# HELP nv_gpu_utilization_percent GPU compute utilization\n"
           "# TYPE nv_gpu_utilization_percent gauge\n");
        for (int d = 0; d < n_gpus; d++)
            PM("nv_gpu_utilization_percent{gpu=\"%d\"} %u\n", d, gpus[d].util_gpu);

        PM("# HELP nv_gpu_temperature_celsius GPU temperature\n"
           "# TYPE nv_gpu_temperature_celsius gauge\n");
        for (int d = 0; d < n_gpus; d++)
            PM("nv_gpu_temperature_celsius{gpu=\"%d\"} %u\n", d, gpus[d].temp);

        PM("# HELP nv_gpu_power_watts GPU power draw\n"
           "# TYPE nv_gpu_power_watts gauge\n");
        for (int d = 0; d < n_gpus; d++)
            if (gpus[d].has_power)
                PM("nv_gpu_power_watts{gpu=\"%d\"} %.1f\n", d, gpus[d].power_mw / 1000.0);

        PM("# HELP nv_gpu_clock_mhz GPU clock speed\n"
           "# TYPE nv_gpu_clock_mhz gauge\n");
        for (int d = 0; d < n_gpus; d++) {
            if (gpus[d].clk_gfx)
                PM("nv_gpu_clock_mhz{gpu=\"%d\",type=\"graphics\"} %u\n", d, gpus[d].clk_gfx);
            if (gpus[d].clk_mem)
                PM("nv_gpu_clock_mhz{gpu=\"%d\",type=\"memory\"} %u\n", d, gpus[d].clk_mem);
        }

        PM("# HELP nv_gpu_memory_total_bytes GPU memory total\n"
           "# TYPE nv_gpu_memory_total_bytes gauge\n");
        for (int d = 0; d < n_gpus; d++)
            if (gpus[d].has_mem)
                PM("nv_gpu_memory_total_bytes{gpu=\"%d\"} %llu\n", d, gpus[d].mem_total);

        PM("# HELP nv_gpu_memory_used_bytes GPU memory used\n"
           "# TYPE nv_gpu_memory_used_bytes gauge\n");
        for (int d = 0; d < n_gpus; d++)
            if (gpus[d].has_mem)
                PM("nv_gpu_memory_used_bytes{gpu=\"%d\"} %llu\n", d, gpus[d].mem_used);

        PM("# HELP nv_gpu_fan_speed_percent GPU fan speed\n"
           "# TYPE nv_gpu_fan_speed_percent gauge\n");
        for (int d = 0; d < n_gpus; d++)
            if (gpus[d].has_fan)
                PM("nv_gpu_fan_speed_percent{gpu=\"%d\"} %u\n", d, gpus[d].fan);

        PM("# HELP nv_gpu_encoder_utilization_percent GPU encoder utilization\n"
           "# TYPE nv_gpu_encoder_utilization_percent gauge\n");
        for (int d = 0; d < n_gpus; d++)
            if (gpus[d].has_enc)
                PM("nv_gpu_encoder_utilization_percent{gpu=\"%d\"} %u\n", d, gpus[d].enc);

        PM("# HELP nv_gpu_decoder_utilization_percent GPU decoder utilization\n"
           "# TYPE nv_gpu_decoder_utilization_percent gauge\n");
        for (int d = 0; d < n_gpus; d++)
            if (gpus[d].has_dec)
                PM("nv_gpu_decoder_utilization_percent{gpu=\"%d\"} %u\n", d, gpus[d].dec);

    }

    /* RDMA / InfiniBand */
    if (rdma_available && rdma_count > 0) {
        PM("# HELP nv_rdma_info RDMA port information\n"
           "# TYPE nv_rdma_info gauge\n");
        for (int i = 0; i < rdma_count; i++)
            PM("nv_rdma_info{device=\"%s\",port=\"%d\",state=\"%s\",rate=\"%s\"} 1\n",
               rdma_ports[i].device, rdma_ports[i].port,
               rdma_ports[i].state, rdma_ports[i].rate);

        PM("# HELP nv_rdma_xmit_bytes_total Total bytes transmitted\n"
           "# TYPE nv_rdma_xmit_bytes_total counter\n");
        for (int i = 0; i < rdma_count; i++)
            PM("nv_rdma_xmit_bytes_total{device=\"%s\",port=\"%d\"} %llu\n",
               rdma_ports[i].device, rdma_ports[i].port, rdma_ports[i].xmit_bytes);

        PM("# HELP nv_rdma_recv_bytes_total Total bytes received\n"
           "# TYPE nv_rdma_recv_bytes_total counter\n");
        for (int i = 0; i < rdma_count; i++)
            PM("nv_rdma_recv_bytes_total{device=\"%s\",port=\"%d\"} %llu\n",
               rdma_ports[i].device, rdma_ports[i].port, rdma_ports[i].recv_bytes);

        PM("# HELP nv_rdma_xmit_packets_total Total packets transmitted\n"
           "# TYPE nv_rdma_xmit_packets_total counter\n");
        for (int i = 0; i < rdma_count; i++)
            PM("nv_rdma_xmit_packets_total{device=\"%s\",port=\"%d\"} %llu\n",
               rdma_ports[i].device, rdma_ports[i].port, rdma_ports[i].xmit_pkts);

        PM("# HELP nv_rdma_recv_packets_total Total packets received\n"
           "# TYPE nv_rdma_recv_packets_total counter\n");
        for (int i = 0; i < rdma_count; i++)
            PM("nv_rdma_recv_packets_total{device=\"%s\",port=\"%d\"} %llu\n",
               rdma_ports[i].device, rdma_ports[i].port, rdma_ports[i].recv_pkts);

        PM("# HELP nv_rdma_errors_total Total RDMA errors\n"
           "# TYPE nv_rdma_errors_total counter\n");
        for (int i = 0; i < rdma_count; i++)
            PM("nv_rdma_errors_total{device=\"%s\",port=\"%d\"} %llu\n",
               rdma_ports[i].device, rdma_ports[i].port, rdma_ports[i].errors);

        PM("# HELP nv_rdma_xmit_bytes_per_second Transmit throughput\n"
           "# TYPE nv_rdma_xmit_bytes_per_second gauge\n");
        for (int i = 0; i < rdma_count; i++)
            PM("nv_rdma_xmit_bytes_per_second{device=\"%s\",port=\"%d\"} %.0f\n",
               rdma_ports[i].device, rdma_ports[i].port, rdma_ports[i].xmit_bytes_sec);

        PM("# HELP nv_rdma_recv_bytes_per_second Receive throughput\n"
           "# TYPE nv_rdma_recv_bytes_per_second gauge\n");
        for (int i = 0; i < rdma_count; i++)
            PM("nv_rdma_recv_bytes_per_second{device=\"%s\",port=\"%d\"} %.0f\n",
               rdma_ports[i].device, rdma_ports[i].port, rdma_ports[i].recv_bytes_sec);
    }

pm_done:
    #undef PM
    return off;
}

/* Minimal HTTP handler for a single connection */
static void prom_handle(int fd) {
    /* Set timeouts to prevent stalled clients from blocking the server */
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char req[512];
    int n = (int)recv(fd, req, sizeof(req) - 1, 0);
    if (n <= 0) return;
    req[n] = '\0';

    /* Bearer token auth if configured */
    if (prom_token) {
        char expected[512];
        snprintf(expected, sizeof(expected), "Authorization: Bearer %s", prom_token);
        if (!strstr(req, expected)) {
            static const char resp_401[] =
                "HTTP/1.1 401 Unauthorized\r\n"
                "Content-Type: text/plain\r\n"
                "Connection: close\r\n\r\n"
                "Unauthorized\n";
            send(fd, resp_401, sizeof(resp_401) - 1, MSG_NOSIGNAL);
            return;
        }
    }

    if (strstr(req, "GET /metrics")) {
        if (!prom_body) return;
        int bodylen = format_metrics(prom_body, prom_buf_size);

        char hdr[128];
        int hlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n\r\n", bodylen);

        send(fd, hdr, hlen, MSG_NOSIGNAL);
        send(fd, prom_body, bodylen, MSG_NOSIGNAL);
    } else {
        /* Landing page with link to /metrics */
        static const char resp[] =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Connection: close\r\n\r\n"
            "<html><body><h1>nv-monitor</h1>"
            "<p><a href=\"/metrics\">Metrics</a></p>"
            "</body></html>\n";
        send(fd, resp, sizeof(resp) - 1, MSG_NOSIGNAL);
    }
}

/* Server thread — blocks on poll() with 1s timeout for clean shutdown */
static void *prom_server(void *arg) {
    (void)arg;
    /* Pre-allocate buffers once for the lifetime of the thread */
    prom_buf_size = PROM_BASE_SIZE + (gpu_count * PROM_BYTES_PER_GPU) +
                    (num_cpus * 80) + (MAX_THERMAL_ZONES * 128) + 512;
    prom_body = malloc(prom_buf_size);
    prom_gpus = calloc(gpu_count > 0 ? gpu_count : 1, sizeof(PromGpu));

    while (!g_quit) {
        struct pollfd pfd = { .fd = prom_sock, .events = POLLIN };
        if (poll(&pfd, 1, 1000) <= 0) continue;

        int fd = accept(prom_sock, NULL, NULL);
        if (fd < 0) continue;
        prom_handle(fd);
        close(fd);
    }
    free(prom_body);  prom_body = NULL;
    free(prom_gpus);  prom_gpus = NULL;
    return NULL;
}

static int prom_start(void) {
    if (!prom_port) return 0;

    prom_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (prom_sock < 0) {
        perror("prometheus: socket");
        return -1;
    }

    int opt = 1;
    setsockopt(prom_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons((uint16_t)prom_port),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(prom_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("prometheus: bind");
        close(prom_sock);
        prom_sock = -1;
        return -1;
    }

    if (listen(prom_sock, 4) < 0) {
        perror("prometheus: listen");
        close(prom_sock);
        prom_sock = -1;
        return -1;
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 524288); /* 512 KB — room for large GPU arrays */

    if (pthread_create(&prom_thread, &attr, prom_server, NULL) != 0) {
        perror("prometheus: pthread_create");
        close(prom_sock);
        prom_sock = -1;
        pthread_attr_destroy(&attr);
        return -1;
    }

    pthread_attr_destroy(&attr);
    fprintf(stderr, "Prometheus metrics at http://0.0.0.0:%d/metrics\n", prom_port);
    return 0;
}

static void prom_stop(void) {
    if (prom_sock >= 0) {
        pthread_join(prom_thread, NULL);
        close(prom_sock);
        prom_sock = -1;
    }
}

/* ── Usage ──────────────────────────────────────────────────────────── */

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Options:\n"
        "  -p PORT   Expose Prometheus metrics on PORT (required)\n"
        "  -t TOKEN  Require Bearer token for /metrics (or NV_MONITOR_TOKEN env)\n"
        "  -r MS     Collection interval in milliseconds (default: 1000)\n"
        "  -v        Show version\n"
        "  -h        Show this help\n"
        "\n"
        "Examples:\n"
        "  %s -p 9101                    Prometheus exporter on :9101\n"
        "  %s -p 9101 -r 2000            Collect every 2s\n"
        "\n"
        "Copyright (c) 2026 Paul Gresham Advisory LLC\n"
        "https://github.com/wentbackward/nv-monitor\n",
        prog, prog, prog);
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    setlocale(LC_NUMERIC, "C"); /* Force decimal point for Prometheus exposition format */
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    int opt;
    while ((opt = getopt(argc, argv, "p:t:r:vh")) != -1) {
        switch (opt) {
        case 'p': prom_port = atoi(optarg); break;
        case 't': prom_token = optarg; break;
        case 'r': delay_ms = atoi(optarg); break;
        case 'v': printf("nv-monitor %s\n", VERSION); return 0;
        case 'h': print_usage(argv[0]); return 0;
        default:  print_usage(argv[0]); return 1;
        }
    }

    /* Token: CLI flag takes precedence, then env var */
    if (!prom_token)
        prom_token = getenv("NV_MONITOR_TOKEN");

    if (!prom_port) {
        fprintf(stderr, "Error: -p <port> is required\n");
        return 1;
    }
    if (delay_ms < 250) delay_ms = 250;

    /* Load NVML */
    nvml_ok = (load_nvml() == 0);
    if (nvml_ok && pNvmlDeviceGetCount)
        pNvmlDeviceGetCount(&gpu_count);

    /* Read CPU core part IDs (for type labels in Prometheus) */
    read_cpu_part_ids();

    /* Detect Tegra GPU sysfs (Jetson fallback) */
    detect_tegra_gpu();
    detect_nic_asic_sensors();
    /* On Tegra/Jetson, NVML returns SUCCESS but zeros for util/temp — prefer sysfs */
    if (tegra_gpu_available)
        use_tegra_gpu = 1;

    /* Detect CPU count and allocate arrays */
    max_cpus = (int)sysconf(_SC_NPROCESSORS_CONF);
    if (max_cpus < 1) max_cpus = 1;
    max_cpus += 16; /* headroom for hotplug */
    prev_ticks = calloc(max_cpus + 1, sizeof(CpuTick));
    cur_ticks  = calloc(max_cpus + 1, sizeof(CpuTick));
    cpu_pct    = calloc(max_cpus + 1, sizeof(double));
    cpu_part   = calloc(max_cpus, sizeof(unsigned int));
    cpu_freq_mhz = calloc(max_cpus + 1, sizeof(int));
    if (!prev_ticks || !cur_ticks || !cpu_pct || !cpu_part || !cpu_freq_mhz) {
        fprintf(stderr, "Failed to allocate CPU arrays for %d cores\n", max_cpus);
        return 1;
    }

    /* Initial CPU tick read */
    read_cpu_ticks(prev_ticks, &num_cpus);
    usleep(100000); /* brief pause for first delta */
    compute_cpu_usage();

    /* Detect RDMA/InfiniBand ports */
    read_net_totals();
    read_rdma_ports();

    /* Start Prometheus exporter */
    if (prom_start() != 0)
        return 1;

    fprintf(stderr, "Running (Ctrl+C to stop)\n");
    while (!g_quit) {
        compute_cpu_usage();
        read_net_totals();
        read_rdma_ports();
        usleep(delay_ms * 1000);
    }
    fprintf(stderr, "\nStopped.\n");

    prom_stop();
    if (nvml_ok && pNvmlShutdown) pNvmlShutdown();
    if (nvml_handle) dlclose(nvml_handle);

    /* Free startup allocations */
    free(prev_ticks); prev_ticks = NULL;
    free(cur_ticks);  cur_ticks = NULL;
    free(cpu_pct);    cpu_pct = NULL;
    free(cpu_part);   cpu_part = NULL;
    free(cpu_freq_mhz); cpu_freq_mhz = NULL;

    return 0;
}
