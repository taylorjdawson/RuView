/**
 * @file ota_update.c
 * @brief HTTP OTA firmware update for ESP32-S3 CSI Node.
 *
 * Uses ESP-IDF's native OTA API with rollback support.
 * The HTTP server runs on port 8032 and accepts:
 *   POST /ota — firmware binary payload (application/octet-stream)
 *   GET /ota/status — current firmware version and partition info
 */

#include "ota_update.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "ota_update";

/** OTA HTTP server port. */
#define OTA_PORT 8032

/** OTA/WASM server stack size.
 * The default HTTPD stack is too small once OTA validation and image parsing
 * run alongside the rest of this firmware stack, which leads to httpd task
 * overflow during large uploads.
 */
#define OTA_HTTPD_STACK_SIZE 12288

/** NVS namespace and key for the OTA pre-shared key. */
#define OTA_NVS_NAMESPACE "security"
#define OTA_NVS_KEY       "ota_psk"

/** Maximum PSK length (hex-encoded SHA-256). */
#define OTA_PSK_MAX_LEN   65

/** Cached PSK loaded from NVS at init time. Empty = auth disabled. */
static char s_ota_psk[OTA_PSK_MAX_LEN] = {0};
static bool s_ota_psk_loaded = false;
static ota_update_quiesce_fn_t s_quiesce_enter_cb = NULL;
static ota_update_quiesce_fn_t s_quiesce_exit_cb = NULL;
static void *s_quiesce_ctx = NULL;

/**
 * Yield frequency during large uploads.
 * A 1-tick delay every 16 KiB lets IDLE0 feed the task watchdog even when the
 * network is fast enough for httpd to otherwise monopolize CPU0.
 */
#define OTA_YIELD_INTERVAL_BYTES (16 * 1024)

static const esp_partition_t *ota_get_update_partition(size_t *max_size_out)
{
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (max_size_out != NULL) {
        *max_size_out = (update != NULL) ? update->size : 0U;
    }
    return update;
}

static void ota_load_psk_from_nvs(void)
{
    if (s_ota_psk_loaded) {
        return;
    }
    s_ota_psk_loaded = true;
    s_ota_psk[0] = '\0';

    nvs_handle_t nvs;
    if (nvs_open(OTA_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(s_ota_psk);
        if (nvs_get_str(nvs, OTA_NVS_KEY, s_ota_psk, &len) == ESP_OK) {
            ESP_LOGI(TAG, "OTA PSK loaded from NVS (%d chars) — authentication enabled",
                     (int)len - 1);
        } else {
            ESP_LOGW(TAG, "No OTA PSK in NVS — OTA authentication DISABLED (provision with nvs_set)");
        }
        nvs_close(nvs);
    } else {
        ESP_LOGW(TAG, "NVS namespace '%s' not found — OTA authentication DISABLED",
                 OTA_NVS_NAMESPACE);
    }
}

esp_err_t ota_update_set_quiesce_hooks(ota_update_quiesce_fn_t enter_cb,
                                       ota_update_quiesce_fn_t exit_cb,
                                       void *ctx)
{
    s_quiesce_enter_cb = enter_cb;
    s_quiesce_exit_cb = exit_cb;
    s_quiesce_ctx = ctx;
    return ESP_OK;
}

static esp_err_t ota_enter_quiesce_mode(void)
{
    if (s_quiesce_enter_cb == NULL) {
        return ESP_OK;
    }

    esp_err_t err = s_quiesce_enter_cb(s_quiesce_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to quiesce runtime for OTA: %s", esp_err_to_name(err));
    }
    return err;
}

static void ota_exit_quiesce_mode(void)
{
    if (s_quiesce_exit_cb == NULL) {
        return;
    }

    esp_err_t err = s_quiesce_exit_cb(s_quiesce_ctx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to restore runtime after OTA abort: %s",
                 esp_err_to_name(err));
    }
}

/**
 * ADR-050: Verify the Authorization header contains the correct PSK.
 * Returns true if auth is disabled (no PSK provisioned) or if the
 * Bearer token matches the stored PSK.
 */
static bool ota_check_auth(httpd_req_t *req)
{
    ota_load_psk_from_nvs();

    if (s_ota_psk[0] == '\0') {
        /* No PSK provisioned — auth disabled (permissive for dev). */
        return true;
    }

    char auth_header[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header,
                                     sizeof(auth_header)) != ESP_OK) {
        return false;
    }

    /* Expect "Bearer <psk>" */
    const char *prefix = "Bearer ";
    if (strncmp(auth_header, prefix, strlen(prefix)) != 0) {
        return false;
    }

    const char *token = auth_header + strlen(prefix);
    /* Constant-time comparison to prevent timing attacks. */
    size_t psk_len = strlen(s_ota_psk);
    size_t tok_len = strlen(token);
    if (psk_len != tok_len) return false;
    volatile uint8_t result = 0;
    for (size_t i = 0; i < psk_len; i++) {
        result |= (uint8_t)(s_ota_psk[i] ^ token[i]);
    }
    return result == 0;
}

bool ota_update_auth_enabled(void)
{
    ota_load_psk_from_nvs();
    return s_ota_psk[0] != '\0';
}

esp_err_t ota_update_require_auth(httpd_req_t *req, const char *action_name)
{
    if (req == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (ota_check_auth(req)) {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "%s rejected: authentication failed",
             (action_name != NULL) ? action_name : "HTTP request");
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
                        "Authentication required. Use: Authorization: Bearer <psk>");
    return ESP_FAIL;
}

/**
 * GET /ota/status — return firmware version and partition info.
 */
static esp_err_t ota_status_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    size_t max_size = 0U;
    const esp_partition_t *update = ota_get_update_partition(&max_size);

    char response[512];
    int len = snprintf(response, sizeof(response),
        "{\"version\":\"%s\",\"date\":\"%s\",\"time\":\"%s\","
        "\"running_partition\":\"%s\",\"next_partition\":\"%s\",\"max_size\":%u}",
        app->version, app->date, app->time,
        running ? running->label : "unknown",
        update ? update->label : "none",
        (unsigned)max_size);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, len);
    return ESP_OK;
}

/**
 * POST /ota — receive and flash firmware binary.
 */
static esp_err_t ota_upload_handler(httpd_req_t *req)
{
    bool runtime_quiesced = false;
    esp_err_t err = ESP_OK;

    /* ADR-050: Authenticate before accepting firmware upload. */
    if (ota_update_require_auth(req, "OTA upload") != ESP_OK) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA update started, content_length=%d", req->content_len);

    size_t max_size = 0U;
    const esp_partition_t *update_partition = ota_get_update_partition(&max_size);
    if (update_partition == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "No OTA partition available");
        return ESP_FAIL;
    }

    if (req->content_len <= 0 || (size_t)req->content_len > max_size) {
        char error[96];
        snprintf(error, sizeof(error),
                 "Invalid firmware size (must be 1B - %u bytes)",
                 (unsigned)max_size);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, error);
        return ESP_FAIL;
    }

    err = ota_enter_quiesce_mode();
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "Unable to quiesce runtime for OTA",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    runtime_quiesced = true;

    esp_ota_handle_t ota_handle;
    err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        ota_exit_quiesce_mode();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "OTA begin failed");
        return ESP_FAIL;
    }

    /* Read firmware in chunks. */
    char buf[1024];
    int received = 0;
    int total = 0;
    int bytes_since_yield = 0;

    while (total < req->content_len) {
        received = httpd_req_recv(req, buf, sizeof(buf));
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;  /* Retry on timeout. */
            }
            ESP_LOGE(TAG, "OTA receive error at byte %d", total);
            esp_ota_abort(ota_handle);
            ota_exit_quiesce_mode();
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "Receive error");
            return ESP_FAIL;
        }

        err = esp_ota_write(ota_handle, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed at byte %d: %s",
                     total, esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            ota_exit_quiesce_mode();
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "OTA write failed");
            return ESP_FAIL;
        }

        total += received;
        bytes_since_yield += received;
        if ((total % (64 * 1024)) == 0) {
            ESP_LOGI(TAG, "OTA progress: %d / %d bytes (%.0f%%)",
                     total, req->content_len,
                     (float)total * 100.0f / (float)req->content_len);
        }
        if (bytes_since_yield >= OTA_YIELD_INTERVAL_BYTES) {
            bytes_since_yield = 0;
            vTaskDelay(1);
        }
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        ota_exit_quiesce_mode();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "OTA validation failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        ota_exit_quiesce_mode();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Set boot partition failed");
        return ESP_FAIL;
    }

    if (runtime_quiesced) {
        ESP_LOGI(TAG, "Runtime workloads quiesced until reboot");
    }
    ESP_LOGI(TAG, "OTA update successful! Rebooting to partition '%s'...",
             update_partition->label);

    const char *resp = "{\"status\":\"ok\",\"message\":\"OTA update successful. Rebooting...\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));

    /* Delay briefly to let the response flush, then reboot. */
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;  /* Never reached. */
}

/** Internal: start the HTTP server and register OTA endpoints. */
static esp_err_t ota_start_server(httpd_handle_t *out_handle)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = OTA_PORT;
    config.max_uri_handlers = 12;  /* Extra slots for WASM endpoints (ADR-040). */
    config.stack_size = OTA_HTTPD_STACK_SIZE;
    /* Increase receive timeout for large uploads. */
    config.recv_wait_timeout = 30;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start OTA HTTP server on port %d: %s",
                 OTA_PORT, esp_err_to_name(err));
        if (out_handle) *out_handle = NULL;
        return err;
    }

    httpd_uri_t status_uri = {
        .uri      = "/ota/status",
        .method   = HTTP_GET,
        .handler  = ota_status_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &status_uri);

    httpd_uri_t upload_uri = {
        .uri      = "/ota",
        .method   = HTTP_POST,
        .handler  = ota_upload_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &upload_uri);

    ESP_LOGI(TAG, "OTA HTTP server started on port %d", OTA_PORT);
    ESP_LOGI(TAG, "  GET  /ota/status — firmware version info");
    ESP_LOGI(TAG, "  POST /ota        — upload new firmware binary");

    if (out_handle) *out_handle = server;
    return ESP_OK;
}

esp_err_t ota_update_init(void)
{
    ota_load_psk_from_nvs();
    return ota_start_server(NULL);
}

esp_err_t ota_update_init_ex(void **out_server)
{
    ota_load_psk_from_nvs();
    return ota_start_server((httpd_handle_t *)out_server);
}
