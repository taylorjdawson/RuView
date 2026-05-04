/**
 * @file piezo.h
 * @brief Configurable piezo speaker driver backed by LEDC PWM.
 *
 * Supports short tones and repeated chirp patterns. Configuration is loaded
 * from Kconfig/NVS so the active GPIO and default tone profile can be changed
 * without editing firmware code.
 */

#ifndef PIEZO_H
#define PIEZO_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define PIEZO_GPIO_DISABLED 255U

typedef struct {
    bool     enabled;              /**< False when the piezo path is disabled. */
    uint8_t  gpio;                 /**< GPIO used for PWM output, or 255 when disabled. */
    uint16_t default_freq_hz;      /**< Default tone frequency in Hz. */
    uint16_t default_duration_ms;  /**< Default tone duration in milliseconds. */
    uint16_t default_gap_ms;       /**< Default gap between repeated chirps. */
    uint8_t  default_duty_pct;     /**< Default PWM duty cycle percentage. */
} piezo_config_t;

typedef struct {
    bool     initialized;      /**< True when LEDC and the worker task are ready. */
    bool     enabled;          /**< True when a usable GPIO has been configured. */
    bool     playing;          /**< True while a tone is actively being driven. */
    uint8_t  gpio;             /**< Configured GPIO, or 255 when disabled. */
    uint32_t current_freq_hz;  /**< Active PWM frequency (0 when idle). */
    uint8_t  current_duty_pct; /**< Active PWM duty cycle (0 when idle). */
} piezo_status_t;

esp_err_t piezo_init(const piezo_config_t *cfg);
esp_err_t piezo_play_tone(uint32_t freq_hz, uint32_t duration_ms, uint8_t duty_pct);
esp_err_t piezo_play_pattern(uint8_t count, uint32_t freq_hz, uint32_t on_ms,
                             uint32_t off_ms, uint8_t duty_pct);
void piezo_stop(void);
void piezo_get_status(piezo_status_t *status);
void piezo_get_default_config(piezo_config_t *cfg);

#endif /* PIEZO_H */
