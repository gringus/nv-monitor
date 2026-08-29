/*
 * Tests for read_thermal_zones() — the per-zone parser used by the
 * Prometheus exporter.
 *
 * Build & run: gcc -O0 -Wall -Wextra -o test_thermal test_thermal.c && ./test_thermal
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

/* Pull in the functions under test (verbatim copies from nv-monitor.c).
 * Only THERMAL_BASE differs — it points at a fixture dir, not /sys. */
#define MAX_THERMAL_ZONES 20
#define THERMAL_BASE      "/tmp/nvmon-tz-test"

static void read_sysfs_str(const char *path, char *buf, int len) {
    FILE *f = fopen(path, "r");
    if (!f) { buf[0] = '\0'; return; }
    if (!fgets(buf, len, f)) buf[0] = '\0';
    fclose(f);
    buf[strcspn(buf, "\n\r")] = '\0';
}

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

int main(void) {
    test_zones_with_gap();
    test_no_zones();
    test_incomplete_zones();

    (void)!system("rm -rf " THERMAL_BASE);
    printf("%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
