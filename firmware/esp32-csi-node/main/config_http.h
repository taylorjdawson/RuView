/**
 * @file config_http.h
 * @brief Remote NVS config + reboot endpoints over the OTA HTTP server.
 *
 * Exposes:
 *   GET  /config/list                              — whitelist of editable keys
 *   POST /config/set?key=<name>&value=<int>        — write to "csi_cfg" NVS
 *   POST /config/reboot                            — schedule reboot in 500ms
 *
 * Writes do not take effect until the device reboots. /config/set is
 * intentionally separate from /config/reboot so a caller can batch
 * multiple sets before applying.
 */

#ifndef CONFIG_HTTP_H
#define CONFIG_HTTP_H

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t config_http_register(httpd_handle_t server);

#endif /* CONFIG_HTTP_H */
