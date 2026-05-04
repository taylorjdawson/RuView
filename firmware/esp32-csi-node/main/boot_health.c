/**
 * @file boot_health.c
 * @brief Persistent crash counter + safe-mode latch (see boot_health.h).
 */

#include "boot_health.h"

#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "boot_health";

#define BOOT_HEALTH_NAMESPACE "boot_health"
#define BOOT_HEALTH_KEY_COUNT "crash_count"

#ifdef CONFIG_BOOT_SAFE_MODE_THRESHOLD
#define BOOT_HEALTH_THRESHOLD CONFIG_BOOT_SAFE_MODE_THRESHOLD
#else
#define BOOT_HEALTH_THRESHOLD 5
#endif

static bool s_boot_safe_mode = false;

static bool boot_reason_is_crash(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
        case ESP_RST_BROWNOUT:
            return true;
        default:
            return false;
    }
}

uint8_t boot_health_record(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    bool crash = boot_reason_is_crash(reason);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(BOOT_HEALTH_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed (%s) — counter disabled this boot",
                 esp_err_to_name(err));
        return 0;
    }

    uint8_t count = 0;
    (void)nvs_get_u8(handle, BOOT_HEALTH_KEY_COUNT, &count);

    if (crash) {
        if (count < UINT8_MAX) {
            count++;
        }
        ESP_LOGW(TAG, "abnormal reset (reason=%d), crash_count=%u/%u",
                 (int)reason, (unsigned)count, (unsigned)BOOT_HEALTH_THRESHOLD);
    } else {
        ESP_LOGI(TAG, "clean reset (reason=%d), crash_count=%u (unchanged)",
                 (int)reason, (unsigned)count);
    }

    if (nvs_set_u8(handle, BOOT_HEALTH_KEY_COUNT, count) == ESP_OK) {
        nvs_commit(handle);
    }
    nvs_close(handle);

    if (count >= BOOT_HEALTH_THRESHOLD) {
        s_boot_safe_mode = true;
        ESP_LOGE(TAG,
                 "SAFE MODE engaged — crash_count=%u >= threshold=%u; "
                 "non-essential subsystems will be skipped to keep OTA recoverable",
                 (unsigned)count, (unsigned)BOOT_HEALTH_THRESHOLD);
    }

    return count;
}

void boot_health_mark_stable(void)
{
    nvs_handle_t handle;
    if (nvs_open(BOOT_HEALTH_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }

    uint8_t count = 0;
    (void)nvs_get_u8(handle, BOOT_HEALTH_KEY_COUNT, &count);
    if (count != 0) {
        if (nvs_set_u8(handle, BOOT_HEALTH_KEY_COUNT, 0) == ESP_OK) {
            nvs_commit(handle);
            ESP_LOGI(TAG, "stability window passed, crash_count cleared (was %u)",
                     (unsigned)count);
        }
    }
    nvs_close(handle);
}

bool boot_health_in_safe_mode(void)
{
    return s_boot_safe_mode;
}
