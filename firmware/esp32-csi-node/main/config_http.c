/**
 * @file config_http.c
 * @brief Remote NVS config + reboot endpoints. See config_http.h.
 */

#include "config_http.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "ota_update.h"

static const char *TAG = "config_http";

#define CONFIG_HTTP_NVS_NAMESPACE "csi_cfg"
#define CONFIG_HTTP_QUERY_BUF     256
#define CONFIG_HTTP_REBOOT_DELAY_MS 500

typedef enum {
    CONFIG_KEY_TYPE_U8,
    CONFIG_KEY_TYPE_U16,
} config_key_type_t;

typedef struct {
    const char *key;
    config_key_type_t type;
    long min_val;
    long max_val;
    /* Sentinel value allowed outside [min,max], or LONG_MIN to disable. */
    long alt_val;
    bool has_alt;
    const char *description;
} config_key_def_t;

static const config_key_def_t s_config_keys[] = {
    { "piezo_gpio", CONFIG_KEY_TYPE_U8,  0,   48,    255, true,
      "Piezo PWM GPIO (0-48, or 255 to disable)" },
    { "piezo_freq", CONFIG_KEY_TYPE_U16, 100, 20000, 0,   false,
      "Piezo default tone frequency in Hz" },
    { "piezo_ms",   CONFIG_KEY_TYPE_U16, 10,  60000, 0,   false,
      "Piezo default duration in ms" },
    { "piezo_gap",  CONFIG_KEY_TYPE_U16, 10,  60000, 0,   false,
      "Piezo default gap between chirps in ms" },
    { "piezo_duty", CONFIG_KEY_TYPE_U8,  1,   90,    0,   false,
      "Piezo default duty cycle percent (1-90)" },

    /* ADR-081: I2S microphone — disabled by default, opt-in per node. */
    { "mic_enable", CONFIG_KEY_TYPE_U8,  0,   1,     0,   false,
      "Enable I2S mic (0=off, 1=on)" },
    { "mic_ws",     CONFIG_KEY_TYPE_U8,  0,   48,    0,   false,
      "Mic WS GPIO (default 2 = D1)" },
    { "mic_sck",    CONFIG_KEY_TYPE_U8,  0,   48,    0,   false,
      "Mic SCK GPIO (default 4 = D3)" },
    { "mic_sd",     CONFIG_KEY_TYPE_U8,  0,   48,    0,   false,
      "Mic SD GPIO (default 5 = D4)" },
    { "mic_sr",     CONFIG_KEY_TYPE_U16, 8000, 48000, 0,  false,
      "Mic sample rate Hz (default 16000)" },
    { "mic_shift",  CONFIG_KEY_TYPE_U8,  8,   16,    0,   false,
      "Mic 32->24 bit shift (default 14)" },
};
#define CONFIG_KEY_COUNT (sizeof(s_config_keys) / sizeof(s_config_keys[0]))

static const config_key_def_t *find_key(const char *name)
{
    for (size_t i = 0; i < CONFIG_KEY_COUNT; i++) {
        if (strcmp(name, s_config_keys[i].key) == 0) {
            return &s_config_keys[i];
        }
    }
    return NULL;
}

static bool get_query_param(httpd_req_t *req, const char *key,
                            char *out, size_t out_len)
{
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0 || qlen >= CONFIG_HTTP_QUERY_BUF || out == NULL || out_len == 0) {
        return false;
    }
    char qbuf[CONFIG_HTTP_QUERY_BUF];
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) != ESP_OK) {
        return false;
    }
    return httpd_query_key_value(qbuf, key, out, out_len) == ESP_OK;
}

static esp_err_t config_set_handler(httpd_req_t *req)
{
    if (ota_update_require_auth(req, "config set") != ESP_OK) {
        return ESP_OK;
    }

    char key_buf[32];
    if (!get_query_param(req, "key", key_buf, sizeof(key_buf))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing key");
    }

    char val_buf[16];
    if (!get_query_param(req, "value", val_buf, sizeof(val_buf))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing value");
    }

    const config_key_def_t *def = find_key(key_buf);
    if (def == NULL) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown key");
    }

    char *end = NULL;
    errno = 0;
    long value = strtol(val_buf, &end, 10);
    if (errno != 0 || end == val_buf || *end != '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "value not an integer");
    }

    bool in_range = (value >= def->min_val && value <= def->max_val);
    if (!in_range && def->has_alt) {
        in_range = (value == def->alt_val);
    }
    if (!in_range) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "value out of range");
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_HTTP_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }

    switch (def->type) {
    case CONFIG_KEY_TYPE_U8:
        err = nvs_set_u8(handle, def->key, (uint8_t)value);
        break;
    case CONFIG_KEY_TYPE_U16:
        err = nvs_set_u16(handle, def->key, (uint16_t)value);
        break;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs write/commit failed for %s: %s", def->key, esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Config set: %s=%ld (reboot required to apply)", def->key, value);

    char resp[160];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"key\":\"%s\",\"value\":%ld,\"reboot_required\":true}",
             def->key, value);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t config_list_handler(httpd_req_t *req)
{
    char resp[1024];
    int written = snprintf(resp, sizeof(resp), "{\"keys\":[");
    for (size_t i = 0; i < CONFIG_KEY_COUNT && written > 0 && written < (int)sizeof(resp); i++) {
        const config_key_def_t *def = &s_config_keys[i];
        const char *type_str = (def->type == CONFIG_KEY_TYPE_U8) ? "u8" : "u16";
        int n;
        if (def->has_alt) {
            n = snprintf(resp + written, sizeof(resp) - written,
                "%s{\"key\":\"%s\",\"type\":\"%s\",\"min\":%ld,\"max\":%ld,\"alt\":%ld,\"desc\":\"%s\"}",
                (i == 0) ? "" : ",", def->key, type_str,
                def->min_val, def->max_val, def->alt_val, def->description);
        } else {
            n = snprintf(resp + written, sizeof(resp) - written,
                "%s{\"key\":\"%s\",\"type\":\"%s\",\"min\":%ld,\"max\":%ld,\"desc\":\"%s\"}",
                (i == 0) ? "" : ",", def->key, type_str,
                def->min_val, def->max_val, def->description);
        }
        if (n < 0) break;
        written += n;
    }
    if (written > 0 && written < (int)sizeof(resp)) {
        snprintf(resp + written, sizeof(resp) - written, "]}");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

static void deferred_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(CONFIG_HTTP_REBOOT_DELAY_MS));
    ESP_LOGW(TAG, "Rebooting now (requested via /config/reboot)");
    esp_restart();
}

static esp_err_t config_reboot_handler(httpd_req_t *req)
{
    if (ota_update_require_auth(req, "config reboot") != ESP_OK) {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Reboot requested via /config/reboot — rebooting in %d ms",
             CONFIG_HTTP_REBOOT_DELAY_MS);

    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"reboot_in_ms\":%d}",
             CONFIG_HTTP_REBOOT_DELAY_MS);
    httpd_resp_set_type(req, "application/json");
    esp_err_t send_err = httpd_resp_sendstr(req, resp);

    /* Schedule reboot from a separate task so the HTTP response flushes first. */
    BaseType_t ok = xTaskCreate(deferred_reboot_task, "reboot_task",
                                2048, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn deferred reboot task — rebooting inline");
        esp_restart();
    }
    return send_err;
}

esp_err_t config_http_register(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    httpd_uri_t set_uri = {
        .uri = "/config/set",    .method = HTTP_POST,
        .handler = config_set_handler,    .user_ctx = NULL,
    };
    httpd_uri_t list_uri = {
        .uri = "/config/list",   .method = HTTP_GET,
        .handler = config_list_handler,   .user_ctx = NULL,
    };
    httpd_uri_t reboot_uri = {
        .uri = "/config/reboot", .method = HTTP_POST,
        .handler = config_reboot_handler, .user_ctx = NULL,
    };

    ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(server, &set_uri));
    ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(server, &list_uri));
    ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(server, &reboot_uri));

    ESP_LOGI(TAG, "Config HTTP control ready:");
    ESP_LOGI(TAG, "  GET  /config/list");
    ESP_LOGI(TAG, "  POST /config/set?key=<name>&value=<int>");
    ESP_LOGI(TAG, "  POST /config/reboot");
    return ESP_OK;
}
