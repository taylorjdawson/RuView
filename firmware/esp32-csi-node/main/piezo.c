/**
 * @file piezo.c
 * @brief Configurable LEDC-backed piezo driver.
 */

#include "piezo.h"

#include <string.h>

#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "piezo";

#define PIEZO_MIN_FREQ_HZ     100U
#define PIEZO_MAX_FREQ_HZ   20000U
#define PIEZO_MIN_DUTY_PCT     1U
#define PIEZO_MAX_DUTY_PCT    90U
#define PIEZO_MIN_MS          10U
#define PIEZO_MAX_MS       60000U
#define PIEZO_QUEUE_LEN        1U
#define PIEZO_TASK_STACK    3072U
#define PIEZO_TASK_PRIO        4U
#define PIEZO_WAIT_SLICE_MS   25U

typedef enum {
    PIEZO_CMD_STOP = 0,
    PIEZO_CMD_TONE = 1,
    PIEZO_CMD_PATTERN = 2,
} piezo_cmd_type_t;

typedef struct {
    piezo_cmd_type_t type;
    uint32_t freq_hz;
    uint32_t on_ms;
    uint32_t off_ms;
    uint8_t  repeat_count;
    uint8_t  duty_pct;
} piezo_cmd_t;

static QueueHandle_t      s_cmd_queue;
static SemaphoreHandle_t  s_status_lock;
static TaskHandle_t       s_worker_task;
static piezo_config_t     s_cfg;
static piezo_status_t     s_status;

static bool piezo_is_enabled_config(const piezo_config_t *cfg)
{
    return cfg != NULL && cfg->enabled && cfg->gpio != PIEZO_GPIO_DISABLED;
}

static bool piezo_valid_freq(uint32_t freq_hz)
{
    return freq_hz >= PIEZO_MIN_FREQ_HZ && freq_hz <= PIEZO_MAX_FREQ_HZ;
}

static bool piezo_valid_duration(uint32_t duration_ms)
{
    return duration_ms >= PIEZO_MIN_MS && duration_ms <= PIEZO_MAX_MS;
}

static bool piezo_valid_duty(uint8_t duty_pct)
{
    return duty_pct >= PIEZO_MIN_DUTY_PCT && duty_pct <= PIEZO_MAX_DUTY_PCT;
}

static void piezo_update_status(bool playing, uint32_t freq_hz, uint8_t duty_pct)
{
    if (s_status_lock == NULL) {
        return;
    }

    if (xSemaphoreTake(s_status_lock, portMAX_DELAY) == pdTRUE) {
        s_status.playing = playing;
        s_status.current_freq_hz = freq_hz;
        s_status.current_duty_pct = duty_pct;
        xSemaphoreGive(s_status_lock);
    }
}

static esp_err_t piezo_start_output(uint32_t freq_hz, uint8_t duty_pct)
{
    if (!s_status.initialized || !s_status.enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!piezo_valid_freq(freq_hz) || !piezo_valid_duty(duty_pct)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, freq_hz) != (uint32_t)freq_hz) {
        ESP_LOGW(TAG, "Requested %lu Hz, LEDC applied different frequency",
                 (unsigned long)freq_hz);
    }

    uint32_t duty = ((1U << LEDC_TIMER_10_BIT) - 1U) * duty_pct / 100U;
    if (duty == 0U) {
        duty = 1U;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
    piezo_update_status(true, freq_hz, duty_pct);
    return ESP_OK;
}

static void piezo_apply_stop(void)
{
    if (!s_status.initialized || !s_status.enabled) {
        piezo_update_status(false, 0, 0);
        return;
    }

    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    piezo_update_status(false, 0, 0);
}

static bool piezo_wait_or_interrupt(uint32_t wait_ms, piezo_cmd_t *next_cmd)
{
    uint32_t remaining = wait_ms;

    while (remaining > 0U) {
        TickType_t slice_ticks = pdMS_TO_TICKS(
            (remaining > PIEZO_WAIT_SLICE_MS) ? PIEZO_WAIT_SLICE_MS : remaining);

        if (xQueueReceive(s_cmd_queue, next_cmd, slice_ticks) == pdTRUE) {
            return true;
        }

        remaining = (remaining > PIEZO_WAIT_SLICE_MS)
            ? (remaining - PIEZO_WAIT_SLICE_MS)
            : 0U;
    }

    return false;
}

static void piezo_worker(void *arg)
{
    (void)arg;

    piezo_cmd_t cmd;
    piezo_cmd_t next_cmd;

    while (1) {
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }

process_cmd:
        if (cmd.type == PIEZO_CMD_STOP) {
            piezo_apply_stop();
            continue;
        }

        uint8_t repeat_count = (cmd.type == PIEZO_CMD_PATTERN) ? cmd.repeat_count : 1U;
        if (repeat_count == 0U) {
            piezo_apply_stop();
            continue;
        }

        for (uint8_t i = 0; i < repeat_count; i++) {
            if (piezo_start_output(cmd.freq_hz, cmd.duty_pct) != ESP_OK) {
                piezo_apply_stop();
                break;
            }

            if (piezo_wait_or_interrupt(cmd.on_ms, &next_cmd)) {
                piezo_apply_stop();
                cmd = next_cmd;
                goto process_cmd;
            }

            piezo_apply_stop();

            if (cmd.type == PIEZO_CMD_PATTERN && i + 1U < repeat_count && cmd.off_ms > 0U) {
                if (piezo_wait_or_interrupt(cmd.off_ms, &next_cmd)) {
                    cmd = next_cmd;
                    goto process_cmd;
                }
            }
        }
    }
}

esp_err_t piezo_init(const piezo_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_cfg, 0, sizeof(s_cfg));
    memset(&s_status, 0, sizeof(s_status));
    s_cfg = *cfg;
    s_status.enabled = piezo_is_enabled_config(cfg);
    s_status.gpio = cfg->gpio;

    if (!s_status.enabled) {
        ESP_LOGI(TAG, "Piezo disabled (set piezo_gpio in NVS or Kconfig to enable)");
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!piezo_valid_freq(cfg->default_freq_hz)
        || !piezo_valid_duration(cfg->default_duration_ms)
        || !piezo_valid_duration(cfg->default_gap_ms)
        || !piezo_valid_duty(cfg->default_duty_pct))
    {
        ESP_LOGE(TAG, "Invalid piezo defaults: gpio=%u freq=%u duration=%u gap=%u duty=%u",
                 (unsigned)cfg->gpio, (unsigned)cfg->default_freq_hz,
                 (unsigned)cfg->default_duration_ms, (unsigned)cfg->default_gap_ms,
                 (unsigned)cfg->default_duty_pct);
        return ESP_ERR_INVALID_ARG;
    }

    if (s_status_lock == NULL) {
        s_status_lock = xSemaphoreCreateMutex();
        if (s_status_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = cfg->default_freq_hz,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        return err;
    }

    ledc_channel_config_t chan_cfg = {
        .gpio_num   = cfg->gpio,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,
        .hpoint     = 0,
    };
    err = ledc_channel_config(&chan_cfg);
    if (err != ESP_OK) {
        return err;
    }

    if (s_cmd_queue == NULL) {
        s_cmd_queue = xQueueCreate(PIEZO_QUEUE_LEN, sizeof(piezo_cmd_t));
        if (s_cmd_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_worker_task == NULL) {
        BaseType_t ok = xTaskCreate(piezo_worker, "piezo_worker",
                                    PIEZO_TASK_STACK, NULL, PIEZO_TASK_PRIO,
                                    &s_worker_task);
        if (ok != pdPASS) {
            s_worker_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    s_status.initialized = true;
    piezo_apply_stop();
    ESP_LOGI(TAG, "Piezo ready on GPIO %u (default=%u Hz, %u ms, %u%% duty)",
             (unsigned)cfg->gpio, (unsigned)cfg->default_freq_hz,
             (unsigned)cfg->default_duration_ms, (unsigned)cfg->default_duty_pct);
    return ESP_OK;
}

esp_err_t piezo_play_tone(uint32_t freq_hz, uint32_t duration_ms, uint8_t duty_pct)
{
    piezo_cmd_t cmd = {
        .type         = PIEZO_CMD_TONE,
        .freq_hz      = (freq_hz != 0U) ? freq_hz : s_cfg.default_freq_hz,
        .on_ms        = (duration_ms != 0U) ? duration_ms : s_cfg.default_duration_ms,
        .off_ms       = 0,
        .repeat_count = 1,
        .duty_pct     = (duty_pct != 0U) ? duty_pct : s_cfg.default_duty_pct,
    };

    if (!s_status.initialized || !s_status.enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!piezo_valid_freq(cmd.freq_hz)
        || !piezo_valid_duration(cmd.on_ms)
        || !piezo_valid_duty(cmd.duty_pct))
    {
        return ESP_ERR_INVALID_ARG;
    }

    return (xQueueOverwrite(s_cmd_queue, &cmd) == pdPASS) ? ESP_OK : ESP_FAIL;
}

esp_err_t piezo_play_pattern(uint8_t count, uint32_t freq_hz, uint32_t on_ms,
                             uint32_t off_ms, uint8_t duty_pct)
{
    piezo_cmd_t cmd = {
        .type         = PIEZO_CMD_PATTERN,
        .freq_hz      = (freq_hz != 0U) ? freq_hz : s_cfg.default_freq_hz,
        .on_ms        = (on_ms != 0U) ? on_ms : s_cfg.default_duration_ms,
        .off_ms       = (off_ms != 0U) ? off_ms : s_cfg.default_gap_ms,
        .repeat_count = count,
        .duty_pct     = (duty_pct != 0U) ? duty_pct : s_cfg.default_duty_pct,
    };

    if (!s_status.initialized || !s_status.enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    if (count == 0U
        || !piezo_valid_freq(cmd.freq_hz)
        || !piezo_valid_duration(cmd.on_ms)
        || !piezo_valid_duration(cmd.off_ms)
        || !piezo_valid_duty(cmd.duty_pct))
    {
        return ESP_ERR_INVALID_ARG;
    }

    return (xQueueOverwrite(s_cmd_queue, &cmd) == pdPASS) ? ESP_OK : ESP_FAIL;
}

void piezo_stop(void)
{
    piezo_cmd_t cmd = {
        .type = PIEZO_CMD_STOP,
    };

    if (s_status.initialized && s_cmd_queue != NULL) {
        xQueueOverwrite(s_cmd_queue, &cmd);
    }
    piezo_apply_stop();
}

void piezo_get_status(piezo_status_t *status)
{
    if (status == NULL) {
        return;
    }

    memset(status, 0, sizeof(*status));

    if (s_status_lock != NULL && xSemaphoreTake(s_status_lock, portMAX_DELAY) == pdTRUE) {
        *status = s_status;
        xSemaphoreGive(s_status_lock);
        return;
    }

    *status = s_status;
}

void piezo_get_default_config(piezo_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    *cfg = s_cfg;
}
