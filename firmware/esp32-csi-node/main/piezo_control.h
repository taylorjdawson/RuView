/**
 * @file piezo_control.h
 * @brief HTTP and USB-serial control surfaces for the piezo driver.
 */

#ifndef PIEZO_CONTROL_H
#define PIEZO_CONTROL_H

#include "esp_err.h"
#include "esp_http_server.h"

void piezo_control_start_serial(void);
esp_err_t piezo_control_register_http(httpd_handle_t server);

#endif /* PIEZO_CONTROL_H */
