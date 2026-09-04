/*
 * Tests for read_thermal_zones() — the per-zone parser used by the
 * Prometheus exporter.
 *
 * Build & run: make test
 */

#include <sys/stat.h> /* mkdir for fixtures */

/* Test the real code from nv-monitor.c, pointing THERMAL_BASE at a
 * fixture dir instead of /sys, and DISKSTATS_PATH at a fixture file. */
#define THERMAL_BASE    "/tmp/nvmon-tz-test"
#define DISKSTATS_PATH  "/tmp/nvmon-diskstats-test"
#define main            nv_monitor_main
#include "../nv-monitor.c"
#undef main

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT_EQ_INT(name, got, expected) do { \
    tests_run++; \
    if ((got) == (expected)) { \
        tests_passed++; \
    } else { \
        printf("FAIL: %s: expected %d, got %d\n", name, (int)(expected), (int)(got)); \
    } \
} while(0)

#define ASSERT_STR(name, got, expected) do { \
    tests_run++; \
    if (strcmp((got), (expected)) == 0) { \
        tests_passed++; \
    } else { \
        printf("FAIL: %s: expected \"%s\", got \"%s\"\n", name, (expected), (got)); \
    } \
} while(0)

#define ASSERT_NEAR(name, got, expected) do { \
    tests_run++; \
    { double g_ = (got), e_ = (expected); \
    if (g_ >= e_ - 1e-9 && g_ <= e_ + 1e-9) { \
        tests_passed++; \
    } else { \
        printf("FAIL: %s: expected %.3f, got %.3f\n", name, e_, g_); \
    } } \
} while(0)

/* Reset the fixture dir, then create zone i with optional type/temp files
 * (a missing temp means the file is not created, like a real sysfs gap). */
static void reset_base(void) {
    (void)!system("rm -rf " THERMAL_BASE);
    (void)!mkdir(THERMAL_BASE, 0755);
}

static void make_zone(int i, const char *type, const char *temp) {
    char dir[128], path[160];
    snprintf(dir, sizeof(dir), THERMAL_BASE "/thermal_zone%d", i);
    if (mkdir(dir, 0755) != 0) return;
    if (type) {
        snprintf(path, sizeof(path), "%s/type", dir);
        FILE *f = fopen(path, "w");
        if (f) { fprintf(f, "%s\n", type); fclose(f); }
    }
    if (temp) {
        snprintf(path, sizeof(path), "%s/temp", dir);
        FILE *f = fopen(path, "w");
        if (f) { fprintf(f, "%s\n", temp); fclose(f); }
    }
}

static void test_zones_with_gap(void) {
    reset_base();
    make_zone(0, "cpu-therm", "45123");
    make_zone(1, "GPU-therm", "60000");
    make_zone(2, "xgmi", "39999");
    /* zone3 intentionally absent */
    make_zone(4, "mem", "50500");

    double temps[MAX_THERMAL_ZONES] = {0};
    char types[MAX_THERMAL_ZONES][64] = {0};
    int n = read_thermal_zones(temps, types);

    ASSERT_EQ_INT("gap: highest index + 1", n, 5);
    ASSERT_STR("gap: type[0]", types[0], "cpu-therm");
    ASSERT_EQ_INT("gap: type[3] empty (skipped)", types[3][0], 0);
    ASSERT_STR("gap: type[4] keeps real index", types[4], "mem");
    ASSERT_NEAR("gap: 45123 mdeg", temps[0], 45.123);
    ASSERT_NEAR("gap: 60000 mdeg", temps[1], 60.0);
    ASSERT_NEAR("gap: 39999 mdeg", temps[2], 39.999);
    ASSERT_NEAR("gap: 50500 mdeg", temps[4], 50.5);
}

static void test_no_zones(void) {
    reset_base();

    double temps[MAX_THERMAL_ZONES] = {1};
    char types[MAX_THERMAL_ZONES][64] = {0};
    int n = read_thermal_zones(temps, types);

    ASSERT_EQ_INT("none: n == 0", n, 0);
}

static void test_incomplete_zones(void) {
    reset_base();
    /* type present but temp file missing -> zone skipped */
    make_zone(0, "cpu-therm", NULL);
    /* temp present but no type file -> zone skipped */
    make_zone(1, NULL, "42000");

    double temps[MAX_THERMAL_ZONES] = {0};
    char types[MAX_THERMAL_ZONES][64] = {0};
    int n = read_thermal_zones(temps, types);

    ASSERT_EQ_INT("incomplete: n == 0", n, 0);
    ASSERT_EQ_INT("incomplete: type[1] empty", types[1][0], 0);
}

/* ── read_diskstats: whole-disk filter + field extraction ───────────── */

static void test_is_whole_disk(void) {
    ASSERT_EQ_INT("sda",      is_whole_disk("sda"),      1);
    ASSERT_EQ_INT("sda1",     is_whole_disk("sda1"),     0);
    ASSERT_EQ_INT("vdb",      is_whole_disk("vdb"),      1);
    ASSERT_EQ_INT("xvda",     is_whole_disk("xvda"),     1);
    ASSERT_EQ_INT("xvda1",    is_whole_disk("xvda1"),    0);
    ASSERT_EQ_INT("nvme0n1",  is_whole_disk("nvme0n1"),  1);
    ASSERT_EQ_INT("nvme0n1p1",is_whole_disk("nvme0n1p1"),0);
    ASSERT_EQ_INT("mmcblk0",  is_whole_disk("mmcblk0"),  1);
    ASSERT_EQ_INT("mmcblk0p1",is_whole_disk("mmcblk0p1"),0);
    ASSERT_EQ_INT("dm-0",     is_whole_disk("dm-0"),     0);
    ASSERT_EQ_INT("loop0",    is_whole_disk("loop0"),    0);
    ASSERT_EQ_INT("sr0",      is_whole_disk("sr0"),      0);
    ASSERT_EQ_INT("zram0",    is_whole_disk("zram0"),    0);
    ASSERT_EQ_INT("md0",      is_whole_disk("md0"),      0);
}

static void test_diskstats(void) {
    FILE *f = fopen(DISKSTATS_PATH, "w");
    if (!f) { ASSERT_EQ_INT("fixture writable", 0, 1); return; }
    fputs(
        "   8       0 sda 100 1 2000 50 300 2 4000 60 0 0 0 0 0 0 0 0 0\n"
        "   8       1 sda1 50 0 1000 20 100 0 2000 20 0 0 0 0 0 0 0 0 0\n"
        " 259       0 nvme0n1 1000 10 40000 100 2000 20 80000 200 0 0 0 0 0 0 0 0 0 0\n"
        " 259       1 nvme0n1p1 500 5 20000 50 1000 10 40000 100 0 0 0 0 0 0 0 0 0 0\n"
        " 253       0 dm-0 10 0 100 1 10 0 100 1 0 0 0 0 0 0 0 0 0 0\n"
        "   7       0 loop0 1 0 2 0 3 0 4 0 0 0 0 0 0 0 0 0 0\n"
        " 179       0 mmcblk0 7 0 100 1 2 0 50 1 0 0 0 0 0 0 0 0 0 0\n"
        " 254       0 zram0 1 0 10 0 1 0 10 0 0 0 0 0 0 0 0 0 0\n"
        "  11       0 sr0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n", f);
    fclose(f);

    DiskIO devs[MAX_DISK_DEVS];
    int n = read_diskstats(devs, MAX_DISK_DEVS);
    ASSERT_EQ_INT("3 whole disks", n, 3);
    if (n == 3) {
        ASSERT_STR("name[0]", devs[0].name, "sda");
        ASSERT_STR("name[1]", devs[1].name, "nvme0n1");
        ASSERT_STR("name[2]", devs[2].name, "mmcblk0");
        ASSERT_EQ_INT("sda reads",    devs[0].reads,    100);
        ASSERT_EQ_INT("sda rsectors", devs[0].rsectors, 2000);
        ASSERT_EQ_INT("sda writes",   devs[0].writes,   300);
        ASSERT_EQ_INT("sda wsectors", devs[0].wsectors, 4000);
        ASSERT_EQ_INT("nvme reads",     devs[1].reads,     1000);
        ASSERT_EQ_INT("nvme wsectors",  devs[1].wsectors,  80000);
        ASSERT_EQ_INT("mmcblk0 reads",  devs[2].reads,     7);
    }
    (void)!system("rm -f " DISKSTATS_PATH);
}

int main(void) {
    test_zones_with_gap();
    test_no_zones();
    test_incomplete_zones();
    test_is_whole_disk();
    test_diskstats();

    (void)!system("rm -rf " THERMAL_BASE);
    printf("%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
