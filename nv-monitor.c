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
#include <locale.h>
#include <sys/sysinfo.h>
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

/* Cumulative clock-limit violation time (nvmlPerfPolicyType 0..5);
 * violationTime in nanoseconds. DEPRECATED in NVML 13 — probes degrade to absent. */
typedef struct {
    unsigned long long referenceTime;
    unsigned long long violationTime;
} nvmlViolationTime_t;

#define NVML_SUCCESS 0
#define NVML_TEMPERATURE_GPU 0
#define NVML_CLOCK_GRAPHICS 0
#define NVML_CLOCK_MEM 2

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
static nvmlReturn_t (*pNvmlDeviceGetPerformanceState)(nvmlDevice_t, int *);
static nvmlReturn_t (*pNvmlDeviceGetCurrClocksThrottleReasons)(nvmlDevice_t, unsigned long long *);
static nvmlReturn_t (*pNvmlDeviceGetTotalEnergyConsumption)(nvmlDevice_t, unsigned long long *);
static nvmlReturn_t (*pNvmlDeviceGetPcieReplayCounter)(nvmlDevice_t, unsigned int *);
static nvmlReturn_t (*pNvmlDeviceGetUUID)(nvmlDevice_t, char *, unsigned int);
static nvmlReturn_t (*pNvmlSystemGetDriverVersion)(char *, unsigned int);
static nvmlReturn_t (*pNvmlDeviceGetViolationStatus)(nvmlDevice_t, int, nvmlViolationTime_t *);

static void *nvml_handle = NULL;
static int   nvml_ok = 0;
static unsigned int gpu_count = 0;      /* number of GPUs detected */

/* ── Constants ──────────────────────────────────────────────────────── */

#define MAX_THERMAL_ZONES 20
#ifndef THERMAL_BASE
#define THERMAL_BASE      "/sys/class/thermal" /* tests/test_thermal.c redefines this before including nv-monitor.c */
#endif

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

/* Command-line options */
static int   prom_port = 0;  /* Prometheus metrics port (0 = not set) */
static const char *prom_token = NULL; /* Bearer token for /metrics auth */
static char  prom_expected_hdr[512] = ""; /* "Authorization: Bearer <token>", built at startup */

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
    LOAD(pNvmlDeviceGetPerformanceState,          "nvmlDeviceGetPerformanceState");
    /* GB10's NVML dropped the *Curr* symbols; *Current* is the modern name */
    LOAD(pNvmlDeviceGetCurrClocksThrottleReasons, "nvmlDeviceGetCurrentClocksThrottleReasons",
        "nvmlDeviceGetCurrClocksThrottleReasons_v2", "nvmlDeviceGetCurrClocksThrottleReasons");
    LOAD(pNvmlDeviceGetTotalEnergyConsumption,    "nvmlDeviceGetTotalEnergyConsumption");
    LOAD(pNvmlDeviceGetPcieReplayCounter,         "nvmlDeviceGetPcieReplayCounter");
    LOAD(pNvmlDeviceGetUUID,                      "nvmlDeviceGetUUID_v2", "nvmlDeviceGetUUID");
    LOAD(pNvmlSystemGetDriverVersion,             "nvmlSystemGetDriverVersion_v2", "nvmlSystemGetDriverVersion");
    LOAD(pNvmlDeviceGetViolationStatus,           "nvmlDeviceGetViolationStatus");
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
    default:    return "unknown"; /* x86 or unmapped part — keep label sets consistent */
    }
}

/* ── sysfs / proc read helpers ──────────────────────────────────────── */

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

/* Read a decimal int; returns 1 on success, 0 otherwise. */
static int read_sysfs_int(const char *path, int *out) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int val = 0, ok = (fscanf(f, "%d", &val) == 1);
    fclose(f);
    if (ok) *out = val;
    return ok;
}

/* ── CPU sampling ───────────────────────────────────────────────────── */

#define CPU_TICK_FMT "%llu %llu %llu %llu %llu %llu %llu %llu"

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
            sscanf(line + 4, CPU_TICK_FMT,
                   &t.user, &t.nice, &t.system, &t.idle,
                   &t.iowait, &t.irq, &t.softirq, &t.steal);
            ticks[0] = t;
        } else {
            int cpunum;
            if (sscanf(line + 3, "%d " CPU_TICK_FMT, &cpunum,
                       &t.user, &t.nice, &t.system, &t.idle,
                       &t.iowait, &t.irq, &t.softirq, &t.steal) == 9 &&
                cpunum + 1 < max_cpus) {
                ticks[cpunum + 1] = t;
                idx = cpunum + 1;
            }
        }
    }
    *n_cpus = idx;
    fclose(f);
}

static unsigned long long tick_total(const CpuTick *t) {
    return t->user + t->nice + t->system + t->idle +
           t->iowait + t->irq + t->softirq + t->steal;
}

static void compute_cpu_usage(void) {
    memset(cur_ticks, 0, (max_cpus + 1) * sizeof(CpuTick));
    int n = 0;
    read_cpu_ticks(cur_ticks, &n);
    num_cpus = n;

    for (int i = 0; i <= n; i++) {
        unsigned long long prev_idle  = prev_ticks[i].idle + prev_ticks[i].iowait;
        unsigned long long cur_idle   = cur_ticks[i].idle + cur_ticks[i].iowait;
        unsigned long long totald = tick_total(&cur_ticks[i]) - tick_total(&prev_ticks[i]);
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
    for (int i = 1; i <= num_cpus; i++) {
        char path[64];
        int khz;
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i - 1);
        if (read_sysfs_int(path, &khz))
            cpu_freq_mhz[i] = khz / 1000;
    }
}

/* ── Tegra GPU sysfs fallback (Jetson Orin / Nano / NX / AGX) ──────── */

static int tegra_gpu_available = 0;
static char tegra_gpu_load_path[256] = "";

/* Thermal zone type names that mean "GPU" on Tegra (matched per scrape) */
static const char *const tegra_gpu_zone_names[] = { "GPU-therm", "gpu-thermal", NULL };

static void detect_tegra_gpu(void) {
    /* Try known Tegra GPU load paths */
    const char *gpu_paths[] = {
        "/sys/devices/gpu.0/load",
        "/sys/devices/platform/bus@0/17000000.gpu/load",
        "/sys/devices/platform/17000000.gpu/load",
        NULL
    };
    for (int i = 0; gpu_paths[i]; i++) {
        int v;
        if (read_sysfs_int(gpu_paths[i], &v)) {
            tegra_gpu_available = 1;
            snprintf(tegra_gpu_load_path, sizeof(tegra_gpu_load_path), "%s", gpu_paths[i]);
            break;
        }
    }
}

static int read_tegra_gpu_util(void) {
    int load;
    if (!read_sysfs_int(tegra_gpu_load_path, &load)) return -1;
    return load / 10; /* scale is 0-1000 -> 0-100% */
}

/* ── NIC ASIC temperature (ConnectX / mlx5) ─────────────────────────── */

#define MAX_NIC_SENSORS   8
#define MAX_DRIVE_SENSORS 8

static char nic_sensor_paths[MAX_NIC_SENSORS][128];
static int  nic_sensor_count = 0;
static char drive_sensor_paths[MAX_DRIVE_SENSORS][128];
static char drive_sensor_labels[MAX_DRIVE_SENSORS][64];
static int  drive_sensor_count = 0;

/* Discover hwmon temperature sensors: mlx5 NIC ASICs and NVMe/HDD drives.
 * Drive labels come from the hwmon device symlink (e.g. ".../nvme/nvme0" -> "nvme0"). */
static void detect_hwmon_sensors(void) {
    DIR *dir = opendir("/sys/class/hwmon");
    if (!dir) return;

    struct dirent *ent = NULL;
    while ((ent = readdir(dir))) {
        if (strncmp(ent->d_name, "hwmon", 5) != 0) continue;

        char path[192], name[64] = "";
        snprintf(path, sizeof(path), "/sys/class/hwmon/%s/name", ent->d_name);
        read_sysfs_str(path, name, sizeof(name));

        int is_nic   = strcmp(name, "mlx5") == 0;
        int is_drive = strcmp(name, "nvme") == 0 || strcmp(name, "drivetemp") == 0;
        if (!is_nic && !is_drive) continue;

        /* Only record sensors we can actually read */
        snprintf(path, sizeof(path), "/sys/class/hwmon/%s/temp1_input", ent->d_name);
        int v;
        if (!read_sysfs_int(path, &v)) continue;

        if (is_nic && nic_sensor_count < MAX_NIC_SENSORS) {
            snprintf(nic_sensor_paths[nic_sensor_count],
                     sizeof(nic_sensor_paths[0]), "%s", path);
            nic_sensor_count++;
        } else if (is_drive && drive_sensor_count < MAX_DRIVE_SENSORS) {
            snprintf(drive_sensor_paths[drive_sensor_count],
                     sizeof(drive_sensor_paths[0]), "%s", path);
            char link[192], resolved[256];
            snprintf(link, sizeof(link), "/sys/class/hwmon/%s/device", ent->d_name);
            ssize_t rlen = readlink(link, resolved, sizeof(resolved) - 1);
            if (rlen > 0) {
                resolved[rlen] = '\0';
                const char *base = strrchr(resolved, '/');
                snprintf(drive_sensor_labels[drive_sensor_count],
                         sizeof(drive_sensor_labels[0]), "%s", base ? base + 1 : resolved);
            } else {
                snprintf(drive_sensor_labels[drive_sensor_count],
                         sizeof(drive_sensor_labels[0]), "%s", ent->d_name);
            }
            drive_sensor_count++;
        }
    }
    closedir(dir);
}

/* Deg C from a hwmon temp*_input path, 0 if unreadable */
static int read_hwmon_temp(const char *path) {
    int millideg;
    return read_sysfs_int(path, &millideg) ? millideg / 1000 : 0;
}

/* Return hottest ASIC temperature in deg C, 0 if no sensor found */
static int read_nic_asic_temp(void) {
    int max_temp = 0;
    for (int i = 0; i < nic_sensor_count; i++) {
        int t = read_hwmon_temp(nic_sensor_paths[i]);
        if (t > max_temp) max_temp = t;
    }
    return max_temp;
}

/* ── Network counters (emitted as Prometheus *_total) ───────────────── */

#define MAX_NET_DEVS 32
#ifndef NETDEV_PATH
#define NETDEV_PATH "/proc/net/dev" /* tests/test_thermal.c redefines this */
#endif

typedef struct {
    char name[64];
    unsigned long long rx_bytes, rx_packets, rx_errs, rx_drop;
    unsigned long long tx_bytes, tx_packets, tx_errs, tx_drop;
} NetDev;

/* Read per-interface counters (skips loopback). Returns count filled. */
static int read_net_devices(NetDev *devs, int max) {
    FILE *f = fopen(NETDEV_PATH, "r");
    if (!f) return 0;

    int n = 0;
    char line[512];
    int line_no = 0;
    while (fgets(line, sizeof(line), f)) {
        line_no++;
        if (line_no <= 2) continue;

        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';

        char *p = line;
        while (*p == ' ') p++;
        if (strcmp(p, "lo") == 0) continue;
        if (n >= max) continue;

        /* v1 fields: rx bytes packets errs drop fifo frame compressed multicast,
         * then tx bytes packets errs drop fifo colls carrier compressed */
        unsigned long long v[16];
        if (sscanf(colon + 1,
                   " %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
                   &v[8], &v[9], &v[10], &v[11], &v[12], &v[13], &v[14], &v[15]) == 16) {
            snprintf(devs[n].name, sizeof(devs[n].name), "%s", p);
            devs[n].rx_bytes   = v[0];
            devs[n].rx_packets = v[1];
            devs[n].rx_errs    = v[2];
            devs[n].rx_drop    = v[3];
            devs[n].tx_bytes   = v[8];
            devs[n].tx_packets = v[9];
            devs[n].tx_errs    = v[10];
            devs[n].tx_drop    = v[11];
            n++;
        }
    }
    fclose(f);

    return n;
}

/* ── Disk I/O counters (/proc/diskstats, whole devices only) ────────── */

#define MAX_DISK_DEVS 32
#ifndef DISKSTATS_PATH
#define DISKSTATS_PATH "/proc/diskstats" /* tests/test_thermal.c redefines this */
#endif

typedef struct {
    char name[64];
    unsigned long long reads, writes;      /* completed ops */
    unsigned long long rsectors, wsectors; /* 512-byte sectors */
} DiskIO;

/* Whole physical disks only: sda yes, sda1 no, nvme0n1 yes, nvme0n1p1 no,
 * and nothing virtual (loop/ram/dm-/zram/sr/fd). */
static int is_whole_disk(const char *name) {
    char extra;
    /* %c matches only if there are leftover chars ("p1"/"1" partitions);
     * at end-of-string it yields EOF (-1). */
    if (strncmp(name, "nvme", 4) == 0)
        return sscanf(name, "nvme%*un%*u%c", &extra) <= 0;
    if (strncmp(name, "mmcblk", 6) == 0)
        return sscanf(name, "mmcblk%*u%c", &extra) <= 0;
    if (strncmp(name, "loop", 4) == 0 || strncmp(name, "ram", 3) == 0 ||
        strncmp(name, "dm-", 3) == 0 || strncmp(name, "zram", 4) == 0 ||
        strncmp(name, "sr", 2) == 0 || strncmp(name, "fd", 2) == 0)
        return 0;
    /* sd/hd/vd: whole disk has only letters after the 2-char prefix;
     * xvd likewise after its 3 chars. Trailing digits mean a partition. */
    int plen = (strncmp(name, "xvd", 3) == 0) ? 3 :
               (strncmp(name, "sd", 2) == 0 || strncmp(name, "hd", 2) == 0 ||
                strncmp(name, "vd", 2) == 0) ? 2 : 0;
    if (!plen) return 0;
    if (!name[plen]) return 0; /* prefix alone is not a disk */
    for (const char *p = name + plen; *p; p++)
        if (*p < 'a' || *p > 'z')
            return 0;
    return 1;
}

static int read_diskstats(DiskIO *devs, int max) {
    FILE *f = fopen(DISKSTATS_PATH, "r");
    if (!f) return 0;

    int n = 0;
    char line[256];
    while (fgets(line, sizeof(line), f) && n < max) {
        unsigned long long reads, rsect, writes, wsect, d1, d2, d3;
        /* fields: f4=reads f6=rsectors f8=writes f10=wsectors (merged/ms skipped) */
        if (sscanf(line, "%*d %*d %63s %llu %llu %llu %llu %llu %llu %llu",
                   devs[n].name, &reads, &d1, &rsect, &d2, &writes, &d3, &wsect) != 8)
            continue;
        if (!is_whole_disk(devs[n].name))
            continue;
        devs[n].reads    = reads;
        devs[n].writes   = writes;
        devs[n].rsectors = rsect;
        devs[n].wsectors = wsect;
        n++;
    }
    fclose(f);
    return n;
}

/* ── RDMA types (used by Prometheus) ───────────────── */

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
} RdmaPort;

static RdmaPort rdma_ports[MAX_RDMA_PORTS];
static int       rdma_count = 0;

/* ── RDMA / InfiniBand monitoring ───────────────────────────────────── */

static void read_rdma_ports(void) {
    DIR *ib_dir = opendir("/sys/class/infiniband");
    if (!ib_dir) { rdma_count = 0; return; }

    int idx = 0;
    struct dirent *dev_ent;
    while ((dev_ent = readdir(ib_dir)) && idx < MAX_RDMA_PORTS) {
        if (dev_ent->d_name[0] == '.') continue;

        /* Scan ports (typically 1-2) */
        for (int p = 1; p <= 2 && idx < MAX_RDMA_PORTS; p++) {
            char base[192], path[256];
            snprintf(base, sizeof(base), "/sys/class/infiniband/%s/ports/%d",
                     dev_ent->d_name, p);

            RdmaPort *r = &rdma_ports[idx];
            snprintf(r->device, sizeof(r->device), "%s", dev_ent->d_name);
            r->port = p;

            snprintf(path, sizeof(path), "%s/state", base);
            read_sysfs_str(path, r->state, sizeof(r->state));
            if (!r->state[0]) continue; /* no such port */
            /* Strip numeric prefix like "4: ACTIVE" -> "ACTIVE" */
            char *colon = strchr(r->state, ':');
            if (colon) {
                const char *s = colon + 1;
                while (*s == ' ') s++;
                memmove(r->state, s, strlen(s) + 1);
            }

            snprintf(path, sizeof(path), "%s/rate", base);
            read_sysfs_str(path, r->rate, sizeof(r->rate));

            /* Data counters — data counters are in units of 4 bytes (32-bit words) */
            const char *data_counters[] = {
                "port_xmit_data", "port_rcv_data",
                "port_xmit_packets", "port_rcv_packets"
            };
            unsigned long long vals[4];
            for (int c = 0; c < 4; c++) {
                snprintf(path, sizeof(path), "%s/counters/%s", base, data_counters[c]);
                vals[c] = read_sysfs_ull(path);
            }
            r->xmit_bytes = vals[0] * 4;
            r->recv_bytes = vals[1] * 4;
            r->xmit_pkts  = vals[2];
            r->recv_pkts  = vals[3];

            /* Sum error counters */
            r->errors = 0;
            const char *err_counters[] = {
                "symbol_error_counter", "port_rcv_errors",
                "port_rcv_constraint_errors", "port_xmit_constraint_errors",
                "link_error_recovery_counter", "link_downed_counter",
                NULL
            };
            for (int e = 0; err_counters[e]; e++) {
                snprintf(path, sizeof(path), "%s/counters/%s", base, err_counters[e]);
                r->errors += read_sysfs_ull(path);
            }

            idx++;
        }
    }
    closedir(ib_dir);
    rdma_count = idx;
}

/* ── Disks ──────────────────────────────────────────────────────────── */

#define MAX_DISKS 64

typedef struct {
    char mount[256];
    char fstype[32];
    char device[128];
    unsigned long long total;
    unsigned long long avail;
    unsigned long long used;
} Disk;

/* Pseudo / virtual filesystems to skip (kernel-internal, not disk-backed) */
static const char *const pseudo_fs[] = {
    "tmpfs", "devtmpfs", "overlay", "squashfs", "proc", "sysfs",
    "cgroup", "cgroup2", "devpts", "mqueue", "hugetlbfs", "debugfs",
    "tracefs", "fusectl", "configfs", "pstore", "bpf", "autofs",
    "binfmt_misc", "rpc_pipefs", "nsfs", "securityfs", "efivarfs", NULL
};

/* Collect real device-backed mounts. Returns count filled. */
static int read_disks(Disk *disks, int max) {
    FILE *mf = setmntent("/proc/mounts", "r");
    if (!mf) return 0;

    int n = 0;
    struct mntent *me;
    while ((me = getmntent(mf)) != NULL && n < max) {
        /* Only real device-backed mounts (filters out most pseudo fs) */
        if (me->mnt_fsname[0] != '/') continue;
        if (strncmp(me->mnt_fsname, "/dev/loop", 9) == 0) continue;
        int skip = 0;
        for (int i = 0; pseudo_fs[i]; i++)
            if (strcmp(me->mnt_type, pseudo_fs[i]) == 0) { skip = 1; break; }
        if (skip) continue;

        struct statvfs sv;
        if (statvfs(me->mnt_dir, &sv) != 0) continue;

        unsigned long long total = (unsigned long long)sv.f_blocks * sv.f_frsize;
        if (total == 0) continue;

        snprintf(disks[n].mount, sizeof(disks[n].mount), "%s", me->mnt_dir);
        snprintf(disks[n].fstype, sizeof(disks[n].fstype), "%s", me->mnt_type);
        snprintf(disks[n].device, sizeof(disks[n].device), "%s", me->mnt_fsname);
        disks[n].total = total;
        disks[n].avail = (unsigned long long)sv.f_bavail * sv.f_frsize;
        disks[n].used  = total - (unsigned long long)sv.f_bfree * sv.f_frsize;
        n++;
    }
    endmntent(mf);
    return n;
}

/* ── Prometheus metrics exporter ────────────────────────────────────── */

static int   prom_sock = -1;
static pthread_t prom_thread;

#define PROM_BUF_SIZE (1 << 18) /* 256 KB fixed budget, allocated once at server start */

typedef struct {
    char     name[96];
    char     uuid[96];
    unsigned int util_gpu, util_mem;
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
    unsigned int pstate;
    int      has_pstate;
    unsigned long long throttle;   /* raw clocks-event-reasons bitmask */
    int      has_throttle;
    unsigned long long energy_mj;
    int      has_energy;
    unsigned int replay;
    int      has_replay;
    unsigned long long viol_ns[6]; /* per nvmlPerfPolicyType 0..5 */
    int      has_viol[6];
} PromGpu;

/* Label names for nvmlPerfPolicyType 0..5 (policy indices are the array order) */
static const char *const viol_reasons[6] = {
    "sw_power_cap", "sw_thermal_slowdown", "sync_boost",
    "board_limit", "low_utilization", "reliability"
};

static char driver_ver[80] = ""; /* set once at startup when NVML is present */

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
        int millideg;
        snprintf(path, sizeof(path), THERMAL_BASE "/thermal_zone%d/temp", i);
        if (read_sysfs_int(path, &millideg)) {
            temps[i] = millideg / 1000.0;
            n = i + 1;
        }
    }
    return n;
}

/* Format all metrics into buf. Returns bytes written. */
static int format_metrics(char *buf, int buflen) {
    int off = 0;
    static int trunc_warned = 0;

    #define PM(...) do { \
        int _n = snprintf(buf + off, (size_t)(buflen - off), __VA_ARGS__); \
        if (_n > 0) { \
            if (_n >= buflen - off) { \
                off = buflen - 1; \
                if (!trunc_warned) { \
                    fprintf(stderr, "warning: metrics truncated at %d bytes — raise PROM_BUF_SIZE\n", buflen); \
                    trunc_warned = 1; \
                } \
                goto pm_done; \
            } \
            off += _n; \
        } \
    } while(0)

    /* Build info (driver from NVML when present) */
    PM("# HELP nv_build_info nv-monitor version\n"
       "# TYPE nv_build_info gauge\n"
       "nv_build_info{version=\"%s\",driver=\"%s\"} 1\n", VERSION, driver_ver);

    /* Uptime */
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        PM("# HELP nv_uptime_seconds System uptime\n"
           "# TYPE nv_uptime_seconds gauge\n"
           "nv_uptime_seconds %ld\n", si.uptime);
    }

    /* Load average */
    double load[3] = {0, 0, 0};
    getloadavg(load, 3);
    PM("# HELP nv_load_average System load average\n"
       "# TYPE nv_load_average gauge\n"
       "nv_load_average{interval=\"1m\"} %.2f\n"
       "nv_load_average{interval=\"5m\"} %.2f\n"
       "nv_load_average{interval=\"15m\"} %.2f\n", load[0], load[1], load[2]);

    /* CPU usage — delta since the previous scrape */
    compute_cpu_usage();
    PM("# HELP nv_cpu_usage_percent CPU utilization\n"
       "# TYPE nv_cpu_usage_percent gauge\n");
    for (int i = 1; i <= num_cpus; i++)
        PM("nv_cpu_usage_percent{cpu=\"%d\",type=\"%s\"} %.1f\n",
           i - 1, cpu_part_label(i - 1), cpu_pct[i]);

    /* Cumulative CPU time per mode (aggregate since boot).
     * CpuTick is exactly 8 adjacent u64 fields — indexable as an array. */
    {
        static const char *const modes[8] = {
            "user", "nice", "system", "idle", "iowait", "irq", "softirq", "steal"
        };
        const unsigned long long *agg = (const unsigned long long *)&cur_ticks[0];
        double hz = (double)sysconf(_SC_CLK_TCK);
        PM("# HELP nv_cpu_seconds_total Cumulative CPU time per mode\n"
           "# TYPE nv_cpu_seconds_total counter\n");
        for (int m = 0; m < 8; m++)
            PM("nv_cpu_seconds_total{mode=\"%s\"} %.1f\n", modes[m], agg[m] / hz);
    }

    /* Per-thermal-zone temperatures (zone index + kernel zone type) */
    double tz_temp[MAX_THERMAL_ZONES] = {0};
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

    /* CPU frequency */
    read_cpu_freqs();
    PM("# HELP nv_cpu_frequency_mhz CPU frequency\n"
       "# TYPE nv_cpu_frequency_mhz gauge\n");
    for (int i = 1; i <= num_cpus; i++)
        PM("nv_cpu_frequency_mhz{cpu=\"%d\",type=\"%s\"} %d\n",
           i - 1, cpu_part_label(i - 1), cpu_freq_mhz[i]);

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

    NetDev net_devs[MAX_NET_DEVS];
    int n_net = read_net_devices(net_devs, MAX_NET_DEVS);
    if (n_net > 0) {
        PM("# HELP nv_network_receive_bytes_total Total bytes received per network interface\n"
           "# TYPE nv_network_receive_bytes_total counter\n");
        for (int i = 0; i < n_net; i++)
            PM("nv_network_receive_bytes_total{device=\"%s\"} %llu\n",
               net_devs[i].name, net_devs[i].rx_bytes);
        PM("# HELP nv_network_transmit_bytes_total Total bytes transmitted per network interface\n"
           "# TYPE nv_network_transmit_bytes_total counter\n");
        for (int i = 0; i < n_net; i++)
            PM("nv_network_transmit_bytes_total{device=\"%s\"} %llu\n",
               net_devs[i].name, net_devs[i].tx_bytes);

        PM("# HELP nv_network_receive_packets_total Total packets received per network interface\n"
           "# TYPE nv_network_receive_packets_total counter\n");
        for (int i = 0; i < n_net; i++)
            PM("nv_network_receive_packets_total{device=\"%s\"} %llu\n",
               net_devs[i].name, net_devs[i].rx_packets);
        PM("# HELP nv_network_transmit_packets_total Total packets transmitted per network interface\n"
           "# TYPE nv_network_transmit_packets_total counter\n");
        for (int i = 0; i < n_net; i++)
            PM("nv_network_transmit_packets_total{device=\"%s\"} %llu\n",
               net_devs[i].name, net_devs[i].tx_packets);

        PM("# HELP nv_network_receive_errors_total Receive errors per network interface\n"
           "# TYPE nv_network_receive_errors_total counter\n");
        for (int i = 0; i < n_net; i++)
            PM("nv_network_receive_errors_total{device=\"%s\"} %llu\n",
               net_devs[i].name, net_devs[i].rx_errs);
        PM("# HELP nv_network_transmit_errors_total Transmit errors per network interface\n"
           "# TYPE nv_network_transmit_errors_total counter\n");
        for (int i = 0; i < n_net; i++)
            PM("nv_network_transmit_errors_total{device=\"%s\"} %llu\n",
               net_devs[i].name, net_devs[i].tx_errs);

        PM("# HELP nv_network_receive_dropped_total Packets dropped on receive per network interface\n"
           "# TYPE nv_network_receive_dropped_total counter\n");
        for (int i = 0; i < n_net; i++)
            PM("nv_network_receive_dropped_total{device=\"%s\"} %llu\n",
               net_devs[i].name, net_devs[i].rx_drop);
        PM("# HELP nv_network_transmit_dropped_total Packets dropped on transmit per network interface\n"
           "# TYPE nv_network_transmit_dropped_total counter\n");
        for (int i = 0; i < n_net; i++)
            PM("nv_network_transmit_dropped_total{device=\"%s\"} %llu\n",
               net_devs[i].name, net_devs[i].tx_drop);
    }

    int nic_temp = read_nic_asic_temp();
    if (nic_temp > 0) {
        PM("# HELP nv_nic_asic_temperature_celsius NIC ASIC temperature (mlx5/ConnectX, hottest sensor)\n"
           "# TYPE nv_nic_asic_temperature_celsius gauge\n"
           "nv_nic_asic_temperature_celsius %d\n", nic_temp);
    }

    if (drive_sensor_count > 0) {
        PM("# HELP nv_drive_temperature_celsius NVMe/HDD drive temperature\n"
           "# TYPE nv_drive_temperature_celsius gauge\n");
        for (int i = 0; i < drive_sensor_count; i++) {
            int t = read_hwmon_temp(drive_sensor_paths[i]);
            if (t > 0)
                PM("nv_drive_temperature_celsius{device=\"%s\"} %d\n",
                   drive_sensor_labels[i], t);
        }
    }

    /* Disk usage per real mountpoint */
    Disk disks[MAX_DISKS];
    int n_disks = read_disks(disks, MAX_DISKS);
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

    /* Per-disk I/O counters (whole physical devices, kernel sectors are 512 B) */
    DiskIO io_devs[MAX_DISK_DEVS];
    int n_io = read_diskstats(io_devs, MAX_DISK_DEVS);
    if (n_io > 0) {
        static const char *const io_names[4] = {
            "nv_disk_reads_completed_total", "nv_disk_writes_completed_total",
            "nv_disk_read_bytes_total", "nv_disk_written_bytes_total"
        };
        static const char *const io_help[4] = {
            "Completed read operations", "Completed write operations",
            "Bytes read", "Bytes written"
        };
        for (int m = 0; m < 4; m++) {
            PM("# HELP %s %s per physical disk\n# TYPE %s counter\n",
               io_names[m], io_help[m], io_names[m]);
            for (int i = 0; i < n_io; i++) {
                unsigned long long v = m < 2 ? (m == 0 ? io_devs[i].reads : io_devs[i].writes)
                                             : (m == 2 ? io_devs[i].rsectors : io_devs[i].wsectors) * 512ULL;
                PM("%s{device=\"%s\"} %llu\n", io_names[m], io_devs[i].name, v);
            }
        }
    }

    /* GPU — collect data first, then format grouped by metric family */
    PromGpu *gpus = prom_gpus;
    int n_gpus = 0;

    if (nvml_ok) {
        for (unsigned int d = 0; gpus && d < gpu_count; d++) {
            PromGpu *g = &gpus[n_gpus];
            memset(g, 0, sizeof(*g));
            nvmlDevice_t dev;
            if (pNvmlDeviceGetHandleByIndex(d, &dev) != NVML_SUCCESS) continue;
            pNvmlDeviceGetName(dev, g->name, sizeof(g->name));
            if (pNvmlDeviceGetUUID)
                pNvmlDeviceGetUUID(dev, g->uuid, sizeof(g->uuid));

            if (tegra_gpu_available) {
                /* Tegra sysfs overrides NVML, which returns zeros there */
                int tutil = read_tegra_gpu_util();
                if (tutil >= 0) g->util_gpu = (unsigned int)tutil;
                for (int z = 0; z < tz_max; z++)
                    for (int k = 0; tegra_gpu_zone_names[k]; k++)
                        if (strcasecmp(tz_type[z], tegra_gpu_zone_names[k]) == 0) {
                            g->temp = (unsigned int)tz_temp[z];
                            break;
                        }
            } else {
                if (pNvmlDeviceGetUtilizationRates) {
                    nvmlUtilization_t util = {0};
                    pNvmlDeviceGetUtilizationRates(dev, &util);
                    g->util_gpu = util.gpu;
                    g->util_mem = util.memory;
                }
                if (pNvmlDeviceGetTemperature)
                    pNvmlDeviceGetTemperature(dev, NVML_TEMPERATURE_GPU, &g->temp);
                /* Optional per-platform counters (GB10: throttle yes, energy no) */
                int ps;
                g->has_pstate = (pNvmlDeviceGetPerformanceState &&
                                 pNvmlDeviceGetPerformanceState(dev, &ps) == NVML_SUCCESS);
                if (g->has_pstate) g->pstate = (unsigned int)ps;
                g->has_throttle = (pNvmlDeviceGetCurrClocksThrottleReasons &&
                                   pNvmlDeviceGetCurrClocksThrottleReasons(dev, &g->throttle) == NVML_SUCCESS);
                g->has_energy = (pNvmlDeviceGetTotalEnergyConsumption &&
                                 pNvmlDeviceGetTotalEnergyConsumption(dev, &g->energy_mj) == NVML_SUCCESS);
                g->has_replay = (pNvmlDeviceGetPcieReplayCounter &&
                                 pNvmlDeviceGetPcieReplayCounter(dev, &g->replay) == NVML_SUCCESS);
                if (pNvmlDeviceGetViolationStatus) {
                    for (int v = 0; v < 6; v++) {
                        nvmlViolationTime_t vt = {0};
                        g->has_viol[v] = (pNvmlDeviceGetViolationStatus(dev, v, &vt) == NVML_SUCCESS);
                        if (g->has_viol[v]) g->viol_ns[v] = vt.violationTime;
                    }
                }
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
            PM("nv_gpu_info{gpu=\"%d\",name=\"%s\",uuid=\"%s\"} 1\n", d, gpus[d].name, gpus[d].uuid);

        PM("# HELP nv_gpu_utilization_percent GPU compute utilization\n"
           "# TYPE nv_gpu_utilization_percent gauge\n");
        for (int d = 0; d < n_gpus; d++)
            PM("nv_gpu_utilization_percent{gpu=\"%d\"} %u\n", d, gpus[d].util_gpu);

        /* Meaningless on unified-memory parts (always 0) — omit like gpu_memory_*,
         * which is detected via GetMemoryInfo failing with NOT_SUPPORTED */
        if (gpus[0].has_mem) {
            PM("# HELP nv_gpu_memory_utilization_percent GPU memory controller utilization\n"
               "# TYPE nv_gpu_memory_utilization_percent gauge\n");
            for (int d = 0; d < n_gpus; d++)
                if (gpus[d].has_mem)
                    PM("nv_gpu_memory_utilization_percent{gpu=\"%d\"} %u\n", d, gpus[d].util_mem);
        }

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

        if (gpus[0].has_pstate) {
            PM("# HELP nv_gpu_performance_state GPU performance state (P0=max, higher is more idle)\n"
               "# TYPE nv_gpu_performance_state gauge\n");
            for (int d = 0; d < n_gpus; d++)
                if (gpus[d].has_pstate)
                    PM("nv_gpu_performance_state{gpu=\"%d\"} %u\n", d, gpus[d].pstate);
        }

        if (gpus[0].has_throttle) {
            /* Raw NVML clocks-event-reasons bitmask (same field DCGM exports) */
            PM("# HELP nv_gpu_clocks_event_reasons Active clock throttle reasons (bitmask)\n"
               "# TYPE nv_gpu_clocks_event_reasons gauge\n");
            for (int d = 0; d < n_gpus; d++)
                if (gpus[d].has_throttle)
                    PM("nv_gpu_clocks_event_reasons{gpu=\"%d\"} %llu\n", d, gpus[d].throttle);
        }

        /* Cumulative time clocks were held below application clocks per cause.
         * Unlike the event-reasons bitmask, rate() of this never misses a
         * throttle spike between scrapes. */
        if (gpus[0].has_viol[0]) {
            PM("# HELP nv_gpu_throttle_duration_seconds_total Cumulative time GPU clocks were limited, per cause\n"
               "# TYPE nv_gpu_throttle_duration_seconds_total counter\n");
            for (int d = 0; d < n_gpus; d++)
                for (int v = 0; v < 6; v++)
                    if (gpus[d].has_viol[v])
                        PM("nv_gpu_throttle_duration_seconds_total{gpu=\"%d\",reason=\"%s\"} %.6f\n",
                           d, viol_reasons[v], gpus[d].viol_ns[v] / 1e9);
        }

        if (gpus[0].has_energy) {
            PM("# HELP nv_gpu_energy_millijoules_total Cumulative GPU energy consumed\n"
               "# TYPE nv_gpu_energy_millijoules_total counter\n");
            for (int d = 0; d < n_gpus; d++)
                if (gpus[d].has_energy)
                    PM("nv_gpu_energy_millijoules_total{gpu=\"%d\"} %llu\n", d, gpus[d].energy_mj);
        }

        if (gpus[0].has_replay) {
            PM("# HELP nv_gpu_pcie_replay_total PCIe TLP replay counter (link health)\n"
               "# TYPE nv_gpu_pcie_replay_total counter\n");
            for (int d = 0; d < n_gpus; d++)
                if (gpus[d].has_replay)
                    PM("nv_gpu_pcie_replay_total{gpu=\"%d\"} %u\n", d, gpus[d].replay);
        }
    }

    /* RDMA / InfiniBand */
    read_rdma_ports();
    if (rdma_count > 0) {
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

    /* Bearer token auth if configured (expected header built once in main) */
    if (prom_token) {
        if (!strstr(req, prom_expected_hdr)) {
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
    prom_buf_size = PROM_BUF_SIZE;
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
        goto fail;
    }

    if (listen(prom_sock, 4) < 0) {
        perror("prometheus: listen");
        goto fail;
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 524288); /* 512 KB — room for large GPU arrays */

    if (pthread_create(&prom_thread, &attr, prom_server, NULL) != 0) {
        perror("prometheus: pthread_create");
        pthread_attr_destroy(&attr);
        goto fail;
    }

    pthread_attr_destroy(&attr);
    fprintf(stderr, "Prometheus metrics at http://0.0.0.0:%d/metrics\n", prom_port);
    return 0;

fail:
    close(prom_sock);
    prom_sock = -1;
    return -1;
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
        "  -v        Show version\n"
        "  -h        Show this help\n"
        "\n"
        "Examples:\n"
        "  %s -p 9101                    Prometheus exporter on :9101\n"
        "\n"
        "Copyright (c) 2026 Paul Gresham Advisory LLC\n"
        "https://github.com/wentbackward/nv-monitor\n",
        prog, prog);
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    setlocale(LC_NUMERIC, "C"); /* Force decimal point for Prometheus exposition format */
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    int opt;
    while ((opt = getopt(argc, argv, "p:t:vh")) != -1) {
        switch (opt) {
        case 'p': prom_port = atoi(optarg); break;
        case 't': prom_token = optarg; break;
        case 'v': printf("nv-monitor %s\n", VERSION); return 0;
        case 'h': print_usage(argv[0]); return 0;
        default:  print_usage(argv[0]); return 1;
        }
    }

    /* Token: CLI flag takes precedence, then env var */
    if (!prom_token)
        prom_token = getenv("NV_MONITOR_TOKEN");
    if (prom_token)
        snprintf(prom_expected_hdr, sizeof(prom_expected_hdr),
                 "Authorization: Bearer %s", prom_token);

    if (!prom_port) {
        fprintf(stderr, "Error: -p <port> is required\n");
        return 1;
    }

    /* Load NVML */
    nvml_ok = (load_nvml() == 0);
    if (nvml_ok && pNvmlDeviceGetCount)
        pNvmlDeviceGetCount(&gpu_count);
    if (nvml_ok && pNvmlSystemGetDriverVersion)
        pNvmlSystemGetDriverVersion(driver_ver, sizeof(driver_ver));

    /* Detect Tegra GPU sysfs (Jetson fallback); when detected, the scrape
     * path prefers Tegra sysfs over NVML, which returns zeros there */
    detect_tegra_gpu();
    detect_hwmon_sensors();

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

    /* Read CPU core part IDs (for type labels in Prometheus) */
    read_cpu_part_ids();

    /* Initial CPU tick read (baseline for the first scrape's usage delta) */
    read_cpu_ticks(prev_ticks, &num_cpus);

    /* Start Prometheus exporter */
    if (prom_start() != 0)
        return 1;

    fprintf(stderr, "Running (Ctrl+C to stop)\n");
    while (!g_quit)
        pause(); /* main thread idles; all collection happens in the scrape path */
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
