/**
 * @file boot_health.h
 * @brief Persistent crash counter + safe-mode latch for OTA-safe deployment.
 *
 * Tracks consecutive abnormal resets in NVS so a firmware that crashes during
 * boot doesn't brick a remote node. Once the counter reaches
 * CONFIG_BOOT_SAFE_MODE_THRESHOLD, the firmware skips non-essential
 * subsystems on the current boot and stays reachable on the OTA endpoint.
 */

#ifndef BOOT_HEALTH_H
#define BOOT_HEALTH_H

#include <stdbool.h>
#include <stdint.h>

/**
 * Inspect the reset reason and update the persistent crash counter.
 *
 * Must be called once at the very start of app_main, after nvs_flash_init()
 * and before any other subsystem initialization. Abnormal resets (panic,
 * watchdog, brownout) increment the counter; clean resets (power-on, OTA
 * software reset) leave it untouched.
 *
 * Once the counter reaches CONFIG_BOOT_SAFE_MODE_THRESHOLD the firmware
 * latches into safe mode for this boot — see boot_health_in_safe_mode().
 *
 * @return Crash counter value after this boot (0 on a clean baseline).
 */
uint8_t boot_health_record(void);

/**
 * Clear the persistent crash counter.
 *
 * Call from a delayed esp_timer once the firmware has been running stably
 * for CONFIG_BOOT_STABILITY_DELAY_MS so transient flaps don't accumulate.
 */
void boot_health_mark_stable(void);

/**
 * @return true when the latest boot_health_record() tripped safe mode.
 */
bool boot_health_in_safe_mode(void);

#endif /* BOOT_HEALTH_H */
