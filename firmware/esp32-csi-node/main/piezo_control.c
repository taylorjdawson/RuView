/**
 * @file piezo_control.c
 * @brief Serial and HTTP control interfaces for configurable piezo tones.
 */

#include "piezo_control.h"

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "driver/uart.h"
#if CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED
#include "driver/usb_serial_jtag.h"
#endif
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_update.h"
#include "piezo.h"

static const char *TAG = "piezo_control";

#define PIEZO_CONSOLE_TASK_STACK   4096
#define PIEZO_CONSOLE_TASK_PRIO       3
#define PIEZO_CONSOLE_BUF_LEN       128
#define PIEZO_CONSOLE_RX_BUF_LEN    256

static bool s_serial_started = false;
static bool s_http_registered = false;

typedef enum {
    PIEZO_CONSOLE_BACKEND_UART = 0,
    PIEZO_CONSOLE_BACKEND_USB_SERIAL_JTAG = 1,
} piezo_console_backend_t;

typedef struct {
    piezo_console_backend_t backend;
    const char *name;
    int channel;
} piezo_console_task_cfg_t;

#if CONFIG_ESP_CONSOLE_UART
static piezo_console_task_cfg_t s_uart_console_cfg = {
    .backend = PIEZO_CONSOLE_BACKEND_UART,
    .name = "UART",
    .channel = CONFIG_ESP_CONSOLE_UART_NUM,
};
#endif

#if CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED && (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG)
static piezo_console_task_cfg_t s_usb_serial_jtag_cfg = {
    .backend = PIEZO_CONSOLE_BACKEND_USB_SERIAL_JTAG,
    .name = "USB Serial/JTAG",
    .channel = -1,
};
#endif

static long parse_long_arg(const char *token, long fallback)
{
    if (token == NULL || *token == '\0') {
        return fallback;
    }

    char *end = NULL;
    errno = 0;
    long value = strtol(token, &end, 10);
    if (errno != 0 || end == token || *end != '\0') {
        return fallback;
    }
    return value;
}

static void log_serial_usage(const char *backend_name)
{
    ESP_LOGI(TAG, "%s piezo commands:", backend_name);
    ESP_LOGI(TAG, "  piezo status");
    ESP_LOGI(TAG, "  piezo tone [freq_hz] [duration_ms] [duty_pct]");
    ESP_LOGI(TAG, "  piezo pattern [count] [freq_hz] [on_ms] [off_ms] [duty_pct]");
    ESP_LOGI(TAG, "  piezo stop");
}

static void log_status(void)
{
    piezo_status_t status;
    piezo_config_t cfg;

    piezo_get_status(&status);
    piezo_get_default_config(&cfg);

    ESP_LOGI(TAG,
             "Piezo status: initialized=%s enabled=%s playing=%s gpio=%u current=%luHz/%u%% default=%uHz/%ums gap=%ums/%u%%",
             status.initialized ? "yes" : "no",
             status.enabled ? "yes" : "no",
             status.playing ? "yes" : "no",
             (unsigned)status.gpio,
             (unsigned long)status.current_freq_hz,
             (unsigned)status.current_duty_pct,
             (unsigned)cfg.default_freq_hz,
             (unsigned)cfg.default_duration_ms,
             (unsigned)cfg.default_gap_ms,
             (unsigned)cfg.default_duty_pct);
}

static void handle_serial_command(char *line)
{
    char *saveptr = NULL;
    char *root = strtok_r(line, " \t", &saveptr);
    if (root == NULL || strcmp(root, "piezo") != 0) {
        return;
    }

    char *sub = strtok_r(NULL, " \t", &saveptr);
    if (sub == NULL || strcmp(sub, "status") == 0) {
        log_status();
        return;
    }

    if (strcmp(sub, "stop") == 0) {
        piezo_stop();
        ESP_LOGI(TAG, "Piezo stopped");
        return;
    }

    if (strcmp(sub, "tone") == 0) {
        long freq = parse_long_arg(strtok_r(NULL, " \t", &saveptr), 0);
        long duration_ms = parse_long_arg(strtok_r(NULL, " \t", &saveptr), 0);
        long duty = parse_long_arg(strtok_r(NULL, " \t", &saveptr), 0);

        esp_err_t err = piezo_play_tone((uint32_t)freq, (uint32_t)duration_ms, (uint8_t)duty);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Piezo tone queued");
        } else {
            ESP_LOGW(TAG, "Piezo tone rejected: %s", esp_err_to_name(err));
        }
        return;
    }

    if (strcmp(sub, "pattern") == 0) {
        long count = parse_long_arg(strtok_r(NULL, " \t", &saveptr), 2);
        long freq = parse_long_arg(strtok_r(NULL, " \t", &saveptr), 0);
        long on_ms = parse_long_arg(strtok_r(NULL, " \t", &saveptr), 0);
        long off_ms = parse_long_arg(strtok_r(NULL, " \t", &saveptr), 0);
        long duty = parse_long_arg(strtok_r(NULL, " \t", &saveptr), 0);

        esp_err_t err = piezo_play_pattern((uint8_t)count, (uint32_t)freq,
                                           (uint32_t)on_ms, (uint32_t)off_ms,
                                           (uint8_t)duty);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Piezo pattern queued");
        } else {
            ESP_LOGW(TAG, "Piezo pattern rejected: %s", esp_err_to_name(err));
        }
        return;
    }

    ESP_LOGW(TAG, "Unknown piezo command: %s", sub);
    log_serial_usage("Serial");
}

static int piezo_console_read_byte(const piezo_console_task_cfg_t *cfg, uint8_t *ch)
{
    if (cfg == NULL || ch == NULL) {
        return -1;
    }

    switch (cfg->backend) {
#if CONFIG_ESP_CONSOLE_UART
    case PIEZO_CONSOLE_BACKEND_UART:
        return uart_read_bytes((uart_port_t)cfg->channel, ch, 1, pdMS_TO_TICKS(100));
#endif
#if CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED && (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG)
    case PIEZO_CONSOLE_BACKEND_USB_SERIAL_JTAG:
        return usb_serial_jtag_read_bytes(ch, 1, pdMS_TO_TICKS(100));
#endif
    default:
        return -1;
    }
}

static esp_err_t piezo_console_prepare_backend(const piezo_console_task_cfg_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (cfg->backend) {
#if CONFIG_ESP_CONSOLE_UART
    case PIEZO_CONSOLE_BACKEND_UART:
        if (uart_is_driver_installed((uart_port_t)cfg->channel)) {
            return ESP_OK;
        }

        return uart_driver_install((uart_port_t)cfg->channel,
                                   PIEZO_CONSOLE_RX_BUF_LEN,
                                   0, 0, NULL, 0);
#endif
#if CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED && (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG)
    case PIEZO_CONSOLE_BACKEND_USB_SERIAL_JTAG: {
        usb_serial_jtag_driver_config_t usb_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        esp_err_t err = usb_serial_jtag_driver_install(&usb_cfg);
        if (err == ESP_ERR_INVALID_STATE) {
            return ESP_OK;
        }
        return err;
    }
#endif
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

static void piezo_serial_task(void *arg)
{
    const piezo_console_task_cfg_t *cfg = (const piezo_console_task_cfg_t *)arg;
    if (cfg == NULL) {
        vTaskDelete(NULL);
        return;
    }

    char line[PIEZO_CONSOLE_BUF_LEN];
    size_t len = 0;

    log_serial_usage(cfg->name);

    while (1) {
        uint8_t ch = 0;
        int rc = piezo_console_read_byte(cfg, &ch);
        if (rc <= 0) {
            continue;
        }

        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            if (len > 0) {
                line[len] = '\0';
                handle_serial_command(line);
                len = 0;
            }
            continue;
        }

        if (len + 1 < sizeof(line)) {
            line[len++] = (char)ch;
        } else {
            len = 0;
            ESP_LOGW(TAG, "Dropping overlong serial command");
        }
    }
}

static bool piezo_console_start_backend(piezo_console_task_cfg_t *cfg, const char *task_name)
{
    esp_err_t err = piezo_console_prepare_backend(cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Piezo %s console unavailable: %s",
                 cfg->name, esp_err_to_name(err));
        return false;
    }

    TaskHandle_t task = NULL;
    BaseType_t ok = xTaskCreate(piezo_serial_task, task_name,
                                PIEZO_CONSOLE_TASK_STACK, cfg,
                                PIEZO_CONSOLE_TASK_PRIO, &task);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "Failed to start piezo %s console task", cfg->name);
        return false;
    }

    return true;
}

static bool get_query_value(httpd_req_t *req, const char *key,
                            char *out, size_t out_len)
{
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len == 0 || query_len >= 128 || out == NULL || out_len == 0) {
        return false;
    }

    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }

    return httpd_query_key_value(query, key, out, out_len) == ESP_OK;
}

static uint32_t parse_query_u32(httpd_req_t *req, const char *key)
{
    char buf[16];
    if (!get_query_value(req, key, buf, sizeof(buf))) {
        return 0;
    }
    return (uint32_t)strtoul(buf, NULL, 10);
}

static esp_err_t piezo_status_handler(httpd_req_t *req)
{
    piezo_status_t status;
    piezo_config_t cfg;
    char response[256];

    piezo_get_status(&status);
    piezo_get_default_config(&cfg);

    httpd_resp_set_type(req, "application/json");
    int written = snprintf(
        response, sizeof(response),
        "{\"initialized\":%s,\"enabled\":%s,\"playing\":%s,"
        "\"gpio\":%u,\"current_freq_hz\":%lu,\"current_duty_pct\":%u,"
        "\"default_freq_hz\":%u,\"default_duration_ms\":%u,"
        "\"default_gap_ms\":%u,\"default_duty_pct\":%u}",
        status.initialized ? "true" : "false",
        status.enabled ? "true" : "false",
        status.playing ? "true" : "false",
        (unsigned)status.gpio,
        (unsigned long)status.current_freq_hz,
        (unsigned)status.current_duty_pct,
        (unsigned)cfg.default_freq_hz,
        (unsigned)cfg.default_duration_ms,
        (unsigned)cfg.default_gap_ms,
        (unsigned)cfg.default_duty_pct);

    if (written < 0 || (size_t)written >= sizeof(response)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "piezo status serialization failed");
    }

    return httpd_resp_sendstr(req, response);
}

static esp_err_t piezo_tone_handler(httpd_req_t *req)
{
    if (ota_update_require_auth(req, "piezo tone") != ESP_OK) {
        return ESP_OK;
    }

    esp_err_t err = piezo_play_tone(parse_query_u32(req, "freq"),
                                    parse_query_u32(req, "duration_ms"),
                                    (uint8_t)parse_query_u32(req, "duty"));
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
    }

    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t piezo_pattern_handler(httpd_req_t *req)
{
    if (ota_update_require_auth(req, "piezo pattern") != ESP_OK) {
        return ESP_OK;
    }

    uint32_t count = parse_query_u32(req, "count");
    if (count == 0U) {
        count = 2U;
    }

    esp_err_t err = piezo_play_pattern((uint8_t)count,
                                       parse_query_u32(req, "freq"),
                                       parse_query_u32(req, "on_ms"),
                                       parse_query_u32(req, "off_ms"),
                                       (uint8_t)parse_query_u32(req, "duty"));
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
    }

    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t piezo_stop_handler(httpd_req_t *req)
{
    if (ota_update_require_auth(req, "piezo stop") != ESP_OK) {
        return ESP_OK;
    }

    piezo_stop();
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

void piezo_control_start_serial(void)
{
    if (s_serial_started) {
        return;
    }

    bool started_any = false;

#if CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED && (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG)
    started_any = piezo_console_start_backend(&s_usb_serial_jtag_cfg, "piezo_usb")
        || started_any;
#endif

#if CONFIG_ESP_CONSOLE_UART
    started_any = piezo_console_start_backend(&s_uart_console_cfg, "piezo_uart")
        || started_any;
#endif

    s_serial_started = started_any;
    if (!started_any) {
        ESP_LOGW(TAG, "Piezo serial control disabled: no console backend available");
    }
}

esp_err_t piezo_control_register_http(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_http_registered) {
        return ESP_OK;
    }

    httpd_uri_t status_uri = {
        .uri      = "/piezo/status",
        .method   = HTTP_GET,
        .handler  = piezo_status_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t tone_uri = {
        .uri      = "/piezo/tone",
        .method   = HTTP_POST,
        .handler  = piezo_tone_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t pattern_uri = {
        .uri      = "/piezo/pattern",
        .method   = HTTP_POST,
        .handler  = piezo_pattern_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t stop_uri = {
        .uri      = "/piezo/stop",
        .method   = HTTP_POST,
        .handler  = piezo_stop_handler,
        .user_ctx = NULL,
    };

    ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(server, &status_uri));
    ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(server, &tone_uri));
    ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(server, &pattern_uri));
    ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(server, &stop_uri));

    s_http_registered = true;
    ESP_LOGI(TAG, "Piezo HTTP control ready:");
    ESP_LOGI(TAG, "  GET  /piezo/status");
    ESP_LOGI(TAG, "  POST /piezo/tone?freq=2200&duration_ms=180&duty=40");
    ESP_LOGI(TAG, "  POST /piezo/pattern?count=3&freq=2200&on_ms=120&off_ms=80&duty=40");
    ESP_LOGI(TAG, "  POST /piezo/stop");
    return ESP_OK;
}
