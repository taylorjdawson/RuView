/**
 * @file ota_update.h
 * @brief HTTP OTA firmware update endpoint for ESP32-S3 CSI Node.
 *
 * Provides an HTTP server endpoint that accepts firmware binaries
 * for over-the-air updates without physical access to the device.
 */

#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"

typedef esp_err_t (*ota_update_quiesce_fn_t)(void *ctx);

/**
 * Initialize the OTA update HTTP server.
 * Starts a lightweight HTTP server on port 8032 that accepts
 * POST /ota with a firmware binary payload.
 *
 * @return ESP_OK on success.
 */
esp_err_t ota_update_init(void);

/**
 * Initialize the OTA update HTTP server and return the handle.
 * Same as ota_update_init() but exposes the httpd_handle_t so
 * other modules (e.g. WASM upload) can register additional endpoints.
 *
 * @param out_server  Output: HTTP server handle (may be NULL on failure).
 * @return ESP_OK on success.
 */
esp_err_t ota_update_init_ex(void **out_server);

/**
 * Register callbacks that quiesce and restore runtime workloads around OTA.
 *
 * The enter callback runs before the upload starts. The exit callback runs if
 * OTA aborts before reboot so paused workloads can be restored cleanly.
 *
 * @param enter_cb  Callback invoked before OTA begins (may be NULL).
 * @param exit_cb   Callback invoked when OTA aborts (may be NULL).
 * @param ctx       Opaque pointer passed to both callbacks.
 * @return ESP_OK on success.
 */
esp_err_t ota_update_set_quiesce_hooks(ota_update_quiesce_fn_t enter_cb,
                                       ota_update_quiesce_fn_t exit_cb,
                                       void *ctx);

/**
 * Return true when an OTA Bearer PSK has been provisioned in NVS.
 */
bool ota_update_auth_enabled(void);

/**
 * Enforce the shared OTA/WASM HTTP Bearer auth policy on a request.
 *
 * If no PSK has been provisioned, the request is currently allowed to proceed
 * for backward compatibility. When a PSK exists, the caller must supply a
 * matching `Authorization: Bearer <psk>` header or a 403 response is sent.
 *
 * @param req          Incoming HTTP request.
 * @param action_name  Human-readable action for logging (may be NULL).
 * @return ESP_OK when the request may proceed.
 */
esp_err_t ota_update_require_auth(httpd_req_t *req, const char *action_name);

#endif /* OTA_UPDATE_H */
