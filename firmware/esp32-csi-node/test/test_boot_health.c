/**
 * @file test_boot_health.c
 * @brief Host-mode unit tests for the boot_health crash counter + safe mode.
 *
 * Exercises the real boot_health.c against a writable in-memory NVS stub and
 * a controllable esp_reset_reason() so every reset path can be validated
 * without flashing hardware.
 *
 * Build:
 *   make test_boot_health
 *
 * Run:
 *   ./test_boot_health    (exits 0 on success, prints summary)
 */

#include "esp_stubs.h"
#include "boot_health.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define BOOT_HEALTH_THRESHOLD 5  /* Mirror of CONFIG_BOOT_SAFE_MODE_THRESHOLD. */

static int g_failures = 0;
static int g_passed   = 0;

#define TEST_ASSERT(cond, label) do { \
    if (cond) { \
        g_passed++; \
        printf("  ok   %s\n", label); \
    } else { \
        g_failures++; \
        printf("  FAIL %s   (%s:%d)\n", label, __FILE__, __LINE__); \
    } \
} while (0)

/* ---- Helpers ---- */

static void reset_world(esp_reset_reason_t r)
{
    _test_nvs_clear();
    _test_set_reset_reason(r);
}

/* Manually seed a starting counter into NVS so we can simulate previous
 * crashes without restarting the process. */
static void seed_counter(uint8_t value)
{
    nvs_handle_t h;
    if (nvs_open("boot_health", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "crash_count", value);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ---- Tests ---- */

static void test_clean_reset_does_not_increment(void)
{
    printf("test_clean_reset_does_not_increment\n");
    const esp_reset_reason_t clean[] = {
        ESP_RST_POWERON,
        ESP_RST_SW,
        ESP_RST_EXT,
        ESP_RST_DEEPSLEEP,
        ESP_RST_UNKNOWN,
    };
    for (size_t i = 0; i < sizeof(clean) / sizeof(clean[0]); i++) {
        reset_world(clean[i]);
        seed_counter(2);  /* Simulate "we've already had 2 crashes." */
        uint8_t after = boot_health_record();
        char label[64];
        snprintf(label, sizeof(label), "reason=%d preserves count=2", clean[i]);
        TEST_ASSERT(after == 2, label);
    }
}

static void test_each_crash_reason_increments(void)
{
    printf("test_each_crash_reason_increments\n");
    const esp_reset_reason_t crashy[] = {
        ESP_RST_PANIC,
        ESP_RST_INT_WDT,
        ESP_RST_TASK_WDT,
        ESP_RST_WDT,
        ESP_RST_BROWNOUT,
    };
    for (size_t i = 0; i < sizeof(crashy) / sizeof(crashy[0]); i++) {
        reset_world(crashy[i]);
        seed_counter(0);
        uint8_t after = boot_health_record();
        char label[64];
        snprintf(label, sizeof(label), "reason=%d increments 0->1", crashy[i]);
        TEST_ASSERT(after == 1, label);
    }
}

static void test_counter_persists_across_record_calls(void)
{
    printf("test_counter_persists_across_record_calls\n");
    reset_world(ESP_RST_PANIC);
    seed_counter(0);

    uint8_t v;
    v = boot_health_record(); TEST_ASSERT(v == 1, "1st panic -> 1");

    /* Same process, but simulate a fresh boot by re-reading NVS. */
    _test_set_reset_reason(ESP_RST_PANIC);
    v = boot_health_record(); TEST_ASSERT(v == 2, "2nd panic -> 2");

    _test_set_reset_reason(ESP_RST_TASK_WDT);
    v = boot_health_record(); TEST_ASSERT(v == 3, "task_wdt -> 3");

    /* Clean reset doesn't roll back the counter. */
    _test_set_reset_reason(ESP_RST_SW);
    v = boot_health_record(); TEST_ASSERT(v == 3, "sw reset -> still 3");

    /* But it doesn't trip safe mode either, since 3 < threshold. */
    TEST_ASSERT(boot_health_in_safe_mode() == false, "no safe mode at 3");
}

static void test_mark_stable_clears_counter(void)
{
    printf("test_mark_stable_clears_counter\n");
    reset_world(ESP_RST_PANIC);
    seed_counter(3);

    uint8_t after = boot_health_record();
    TEST_ASSERT(after == 4, "panic from 3 -> 4");

    boot_health_mark_stable();

    /* Verify the underlying NVS got cleared. */
    nvs_handle_t h;
    nvs_open("boot_health", NVS_READWRITE, &h);
    uint8_t stored = 99;
    esp_err_t err = nvs_get_u8(h, "crash_count", &stored);
    TEST_ASSERT(err == ESP_OK && stored == 0, "mark_stable clears NVS to 0");
    nvs_close(h);
}

static void test_threshold_trips_safe_mode(void)
{
    printf("test_threshold_trips_safe_mode\n");
    /* This test must run last because boot_health_in_safe_mode() latches
     * globally and there's intentionally no production reset hook. */
    reset_world(ESP_RST_PANIC);
    seed_counter(BOOT_HEALTH_THRESHOLD - 1);

    /* Pre-condition: latch should still be off from prior tests. */
    TEST_ASSERT(boot_health_in_safe_mode() == false, "latch off pre-trip");

    uint8_t after = boot_health_record();
    char label[64];
    snprintf(label, sizeof(label), "panic from %u -> threshold %u",
             BOOT_HEALTH_THRESHOLD - 1, BOOT_HEALTH_THRESHOLD);
    TEST_ASSERT(after == BOOT_HEALTH_THRESHOLD, label);
    TEST_ASSERT(boot_health_in_safe_mode() == true, "safe mode latched");
}

static void test_counter_saturates_at_uint8_max(void)
{
    printf("test_counter_saturates_at_uint8_max\n");
    /* Even after pathological repeated crashes, counter must not wrap. */
    reset_world(ESP_RST_PANIC);
    seed_counter(255);

    uint8_t after = boot_health_record();
    TEST_ASSERT(after == 255, "counter saturates at 255 (no wrap)");
}

static void test_nvs_open_failure_returns_zero(void)
{
    printf("test_nvs_open_failure_returns_zero\n");
    /* Force NVS_OPEN to fail by exhausting the handle pool, then call
     * boot_health_record. It must return 0 and not crash. */
    _test_nvs_clear();
    _test_set_reset_reason(ESP_RST_PANIC);

    /* Open repeatedly to exhaust the handle table. */
    for (int i = 0; i < 64; i++) {
        nvs_handle_t h;
        if (nvs_open("filler", NVS_READWRITE, &h) != ESP_OK) {
            break;
        }
    }
    uint8_t after = boot_health_record();
    TEST_ASSERT(after == 0, "nvs_open failure -> record returns 0");
}

int main(void)
{
    printf("=== boot_health unit tests ===\n");

    /* All tests below this line must keep the safe-mode latch OFF — the
     * latch is intentionally one-way in production and there's no test
     * hook to reset it. */
    test_clean_reset_does_not_increment();
    test_each_crash_reason_increments();
    test_counter_persists_across_record_calls();
    test_mark_stable_clears_counter();
    test_nvs_open_failure_returns_zero();

    /* This trips the latch — anything below it can assume safe mode == true. */
    test_threshold_trips_safe_mode();

    /* Saturation seeds count=255 which would also trip the latch, so run it
     * after the explicit trip test rather than independently. */
    test_counter_saturates_at_uint8_max();

    printf("\n%d passed, %d failed\n", g_passed, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
