/**
 * @file main.c
 * @brief ESP32-S3 CSI Node — ADR-018 compliant firmware.
 *
 * Initializes NVS, WiFi STA mode, CSI collection, and UDP streaming.
 * CSI frames are serialized in ADR-018 binary format and sent to the
 * aggregator over UDP.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"
#include "esp_app_desc.h"
#include "sdkconfig.h"

#include "audio_mic.h"
#include "boot_health.h"
#include "config_http.h"
#include "csi_collector.h"
#include "stream_sender.h"
#include "nvs_config.h"
#include "edge_processing.h"
#include "ota_update.h"
#include "piezo.h"
#include "piezo_control.h"
#include "power_mgmt.h"
#include "wasm_runtime.h"
#include "wasm_upload.h"
#include "display_task.h"
#include "mmwave_sensor.h"
#include "swarm_bridge.h"
#ifdef CONFIG_CSI_MOCK_ENABLED
#include "mock_csi.h"
#endif

#include "esp_timer.h"

static const char *TAG = "main";

/* ADR-040: WASM timer handle (calls on_timer at configurable interval). */
static esp_timer_handle_t s_wasm_timer;

/* Boot-health stability timer: clears the crash counter after a stable run. */
static esp_timer_handle_t s_stability_timer;

/* WiFi backoff reconnect timer: keeps trying after the initial fast retries
 * are exhausted so unattended nodes recover from AP outages on their own. */
static esp_timer_handle_t s_wifi_reconnect_timer;

/* Runtime configuration (loaded from NVS or Kconfig defaults).
 * Global so other modules (wasm_upload.c) can access pubkey, etc. */
nvs_config_t g_nvs_config;

/* Event group bits */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
#define MAX_RETRY 10

#ifndef CONFIG_WIFI_RECONNECT_BACKOFF_MS
#define CONFIG_WIFI_RECONNECT_BACKOFF_MS 30000
#endif
#ifndef CONFIG_BOOT_STABILITY_DELAY_MS
#define CONFIG_BOOT_STABILITY_DELAY_MS 30000
#endif
#ifndef CONFIG_MAIN_LOOP_WDT_TIMEOUT_S
#define CONFIG_MAIN_LOOP_WDT_TIMEOUT_S 30
#endif

static void wifi_reconnect_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "WiFi backoff reconnect: retrying esp_wifi_connect()");
    s_retry_num = 0;  /* Allow another fast-retry burst. */
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect from backoff failed: %s — re-arming",
                 esp_err_to_name(err));
        if (s_wifi_reconnect_timer != NULL) {
            esp_timer_start_once(s_wifi_reconnect_timer,
                                 (uint64_t)CONFIG_WIFI_RECONNECT_BACKOFF_MS * 1000ULL);
        }
    }
}

static void stability_timer_cb(void *arg)
{
    (void)arg;
    boot_health_mark_stable();
}

static esp_err_t ota_quiesce_runtime(void *ctx)
{
    (void)ctx;
    esp_err_t err = ESP_OK;

#ifndef CONFIG_CSI_MOCK_SKIP_WIFI_CONNECT
    err = csi_collector_pause();
    if (err != ESP_OK) {
        return err;
    }
#endif

#ifdef CONFIG_DISPLAY_ENABLE
    err = display_task_pause();
    if (err != ESP_OK) {
#ifndef CONFIG_CSI_MOCK_SKIP_WIFI_CONNECT
        (void)csi_collector_resume();
#endif
        return err;
    }
#endif

    ESP_LOGI(TAG, "Runtime workloads quiesced for OTA");
    return ESP_OK;
}

static esp_err_t ota_restore_runtime(void *ctx)
{
    (void)ctx;

    esp_err_t first_err = ESP_OK;
    esp_err_t err = ESP_OK;

#ifdef CONFIG_DISPLAY_ENABLE
    err = display_task_resume();
    if (err != ESP_OK && first_err == ESP_OK) {
        first_err = err;
    }
#endif

#ifndef CONFIG_CSI_MOCK_SKIP_WIFI_CONNECT
    err = csi_collector_resume();
    if (err != ESP_OK && first_err == ESP_OK) {
        first_err = err;
    }
#endif

    if (first_err == ESP_OK) {
        ESP_LOGI(TAG, "Runtime workloads restored after OTA abort");
    }
    return first_err;
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying WiFi connection (%d/%d)", s_retry_num, MAX_RETRY);
        } else {
            /* Initial fast retries exhausted. Unblock app_main so the rest
             * of the firmware (OTA server, status logging) can come up, but
             * keep retrying in the background on a slow cadence so the node
             * recovers from AP outages without manual intervention. */
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            if (s_wifi_reconnect_timer != NULL &&
                !esp_timer_is_active(s_wifi_reconnect_timer)) {
                ESP_LOGW(TAG, "WiFi unreachable after %d fast retries — "
                              "scheduling backoff retry in %d ms",
                         MAX_RETRY, CONFIG_WIFI_RECONNECT_BACKOFF_MS);
                esp_timer_start_once(s_wifi_reconnect_timer,
                                     (uint64_t)CONFIG_WIFI_RECONNECT_BACKOFF_MS * 1000ULL);
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        if (s_wifi_reconnect_timer != NULL &&
            esp_timer_is_active(s_wifi_reconnect_timer)) {
            esp_timer_stop(s_wifi_reconnect_timer);
        }
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    /* Create the WiFi backoff reconnect timer before WiFi events can fire. */
    const esp_timer_create_args_t reconnect_timer_args = {
        .callback = wifi_reconnect_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_reconn",
    };
    esp_err_t timer_err = esp_timer_create(&reconnect_timer_args, &s_wifi_reconnect_timer);
    if (timer_err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi reconnect timer create failed: %s — auto-recovery disabled",
                 esp_err_to_name(timer_err));
        s_wifi_reconnect_timer = NULL;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    /* Copy runtime SSID/password from NVS config */
    strncpy((char *)wifi_config.sta.ssid, g_nvs_config.wifi_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, g_nvs_config.wifi_password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.ssid[sizeof(wifi_config.sta.ssid) - 1] = '\0';
    wifi_config.sta.password[sizeof(wifi_config.sta.password) - 1] = '\0';

    /* If password is empty, use open auth */
    if (strlen((char *)wifi_config.sta.password) == 0) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi STA initialized, connecting to SSID: %s", g_nvs_config.wifi_ssid);

    /* Wait for connection */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to WiFi");
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to WiFi after %d retries", MAX_RETRY);
    }
}

void app_main(void)
{
    /* Initialize NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Boot-health bookkeeping — must run before any subsystem init so the
     * counter increments even if a downstream init crashes the firmware. */
    uint8_t boot_crash_count = boot_health_record();
    bool safe_mode = boot_health_in_safe_mode();

    /* Load runtime config (NVS overrides Kconfig defaults) */
    nvs_config_load(&g_nvs_config);

    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "ESP32-S3 CSI Node (ADR-018) — v%s — Node ID: %d (boot crash_count=%u%s)",
             app_desc->version, g_nvs_config.node_id,
             (unsigned)boot_crash_count,
             safe_mode ? ", SAFE MODE" : "");

    /* Subscribe the application task to the task watchdog so a hard hang
     * inside any of the synchronous inits below auto-reboots and increments
     * the crash counter. The IDLE-task TWDT is configured by Kconfig and is
     * already running by this point — we reconfigure it so our timeout +
     * trigger_panic settings stick (esp_task_wdt_init returns
     * ESP_ERR_INVALID_STATE if the SDK already initialized TWDT at boot,
     * silently leaving the SDK defaults in place). */
    esp_task_wdt_config_t twdt_cfg = {
        .timeout_ms = (uint32_t)CONFIG_MAIN_LOOP_WDT_TIMEOUT_S * 1000U,
        .idle_core_mask = (1U << portNUM_PROCESSORS) - 1U,
        .trigger_panic = true,
    };
    esp_err_t wdt_cfg_ret = esp_task_wdt_reconfigure(&twdt_cfg);
    if (wdt_cfg_ret == ESP_ERR_INVALID_STATE) {
        /* Not yet initialized — use init instead. */
        wdt_cfg_ret = esp_task_wdt_init(&twdt_cfg);
    }
    if (wdt_cfg_ret != ESP_OK) {
        ESP_LOGW(TAG, "TWDT (re)configure failed: %s", esp_err_to_name(wdt_cfg_ret));
    } else {
        ESP_LOGI(TAG, "TWDT configured: timeout=%us, trigger_panic=true",
                 (unsigned)CONFIG_MAIN_LOOP_WDT_TIMEOUT_S);
    }
    esp_err_t wdt_add_ret = esp_task_wdt_add(NULL);
    if (wdt_add_ret != ESP_OK && wdt_add_ret != ESP_ERR_INVALID_ARG) {
        /* INVALID_ARG just means the task is already subscribed — fine. */
        ESP_LOGW(TAG, "TWDT add(main) failed: %s", esp_err_to_name(wdt_add_ret));
    }

    piezo_config_t piezo_cfg = {
        .enabled             = (g_nvs_config.piezo_gpio != PIEZO_GPIO_DISABLED),
        .gpio                = g_nvs_config.piezo_gpio,
        .default_freq_hz     = g_nvs_config.piezo_freq_hz,
        .default_duration_ms = g_nvs_config.piezo_duration_ms,
        .default_gap_ms      = g_nvs_config.piezo_gap_ms,
        .default_duty_pct    = g_nvs_config.piezo_duty_pct,
    };
    esp_err_t piezo_ret = piezo_init(&piezo_cfg);
    if (piezo_ret != ESP_OK && piezo_ret != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "Piezo init failed: %s", esp_err_to_name(piezo_ret));
    }
    piezo_control_start_serial();

    /* Initialize WiFi STA (skip entirely under QEMU mock — no RF hardware) */
#ifndef CONFIG_CSI_MOCK_SKIP_WIFI_CONNECT
    wifi_init_sta();
#else
    ESP_LOGI(TAG, "Mock CSI mode: skipping WiFi init (CONFIG_CSI_MOCK_SKIP_WIFI_CONNECT)");
#endif

    /* Initialize UDP sender with runtime target */
#ifdef CONFIG_CSI_MOCK_SKIP_WIFI_CONNECT
    ESP_LOGI(TAG, "Mock CSI mode: skipping UDP sender init (no network)");
#else
    if (stream_sender_init_with(g_nvs_config.target_ip, g_nvs_config.target_port) != 0) {
        ESP_LOGE(TAG, "Failed to initialize UDP sender");
        return;
    }
#endif

    /* Initialize CSI collection */
#ifdef CONFIG_CSI_MOCK_ENABLED
    /* ADR-061: Start mock CSI generator (replaces real WiFi CSI in QEMU) */
    esp_err_t mock_ret = mock_csi_init(CONFIG_CSI_MOCK_SCENARIO);
    if (mock_ret != ESP_OK) {
        ESP_LOGE(TAG, "Mock CSI init failed: %s", esp_err_to_name(mock_ret));
    } else {
        ESP_LOGI(TAG, "Mock CSI active (scenario=%d)", CONFIG_CSI_MOCK_SCENARIO);
    }
#else
    csi_collector_init();

    /* ADR-073: Start multi-frequency channel hopping if configured in NVS.
     * Skipped in safe mode to keep the WiFi stack pinned on a single channel
     * for maximum OTA reachability. */
    if (g_nvs_config.channel_hop_count > 1) {
        if (safe_mode) {
            ESP_LOGW(TAG, "Safe mode: skipping channel hopping setup");
        } else {
            ESP_LOGI(TAG, "Starting channel hopping: %u channels, dwell=%lu ms",
                     (unsigned)g_nvs_config.channel_hop_count,
                     (unsigned long)g_nvs_config.dwell_ms);
            csi_collector_set_hop_table(
                g_nvs_config.channel_list,
                g_nvs_config.channel_hop_count,
                g_nvs_config.dwell_ms);
        }
    }
#endif

    /* ADR-039: Initialize edge processing pipeline. */
    edge_config_t edge_cfg = {
        .tier              = g_nvs_config.edge_tier,
        .presence_thresh   = g_nvs_config.presence_thresh,
        .fall_thresh       = g_nvs_config.fall_thresh,
        .vital_window      = g_nvs_config.vital_window,
        .vital_interval_ms = g_nvs_config.vital_interval_ms,
        .top_k_count       = g_nvs_config.top_k_count,
        .power_duty        = g_nvs_config.power_duty,
    };
    esp_err_t edge_ret = edge_processing_init(&edge_cfg);
    if (edge_ret != ESP_OK) {
        ESP_LOGW(TAG, "Edge processing init failed: %s (continuing without edge DSP)",
                 esp_err_to_name(edge_ret));
    }

    /* Initialize OTA update HTTP server (requires network). */
    httpd_handle_t ota_server = NULL;
#ifndef CONFIG_CSI_MOCK_SKIP_WIFI_CONNECT
    esp_err_t ota_ret = ota_update_init_ex(&ota_server);
    if (ota_ret != ESP_OK) {
        ESP_LOGW(TAG, "OTA server init failed: %s", esp_err_to_name(ota_ret));
    }
#else
    esp_err_t ota_ret = ESP_ERR_NOT_SUPPORTED;
    ESP_LOGI(TAG, "Mock CSI mode: skipping OTA server (no network)");
#endif

    /* Register remote NVS config + reboot endpoints. Available even in safe
     * mode so a stuck node can be reconfigured without USB. */
    if (ota_server != NULL) {
        config_http_register(ota_server);
        piezo_control_register_http(ota_server);
        audio_mic_register_http(ota_server);
    }

    /* ADR-040: Initialize WASM programmable sensing runtime.
     * Skipped in safe mode — the WASM3 runtime allocates from PSRAM and
     * runs uploaded bytecode, both of which are plausible crash sources. */
    esp_err_t wasm_ret = ESP_ERR_NOT_SUPPORTED;
    if (safe_mode) {
        ESP_LOGW(TAG, "Safe mode: skipping WASM runtime init");
    } else {
        wasm_ret = wasm_runtime_init();
    }
    if (wasm_ret != ESP_OK) {
        if (!safe_mode) {
            ESP_LOGW(TAG, "WASM runtime init failed: %s", esp_err_to_name(wasm_ret));
        }
    } else {
        /* Register WASM upload endpoints on the OTA HTTP server. */
        if (ota_server != NULL) {
            wasm_upload_register(ota_server);
        }

        /* Start periodic timer for wasm_runtime_on_timer(). */
        esp_timer_create_args_t timer_args = {
            .callback = (void (*)(void *))wasm_runtime_on_timer,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "wasm_timer",
        };
        esp_err_t timer_ret = esp_timer_create(&timer_args, &s_wasm_timer);
        if (timer_ret == ESP_OK) {
#ifdef CONFIG_WASM_TIMER_INTERVAL_MS
            uint64_t interval_us = (uint64_t)CONFIG_WASM_TIMER_INTERVAL_MS * 1000ULL;
#else
            uint64_t interval_us = 1000000ULL;  /* Default: 1 second. */
#endif
            esp_timer_start_periodic(s_wasm_timer, interval_us);
            ESP_LOGI(TAG, "WASM on_timer() periodic: %llu ms",
                     (unsigned long long)(interval_us / 1000));
        } else {
            ESP_LOGW(TAG, "WASM timer create failed: %s", esp_err_to_name(timer_ret));
        }
    }

    /* ADR-063: Initialize mmWave sensor (auto-detect on UART).
     * Skipped in safe mode — UART driver init has historically been a
     * crash source when no sensor is attached. */
    esp_err_t mmwave_ret = ESP_ERR_NOT_SUPPORTED;
    if (safe_mode) {
        ESP_LOGW(TAG, "Safe mode: skipping mmWave sensor probe");
    } else {
        mmwave_ret = mmwave_sensor_init(-1, -1);  /* -1 = use default GPIO pins */
        if (mmwave_ret == ESP_OK) {
            mmwave_state_t mw;
            if (mmwave_sensor_get_state(&mw)) {
                ESP_LOGI(TAG, "mmWave sensor: %s (caps=0x%04x)",
                         mmwave_type_name(mw.type), mw.capabilities);
            }
        } else {
            ESP_LOGI(TAG, "No mmWave sensor detected (CSI-only mode)");
        }
    }

    /* ADR-066: Initialize swarm bridge to Cognitum Seed (if configured).
     * Skipped in safe mode — the bridge spawns a background HTTP task that
     * has been a source of crashes in the past. */
    esp_err_t swarm_ret = ESP_ERR_INVALID_ARG;
#ifndef CONFIG_CSI_MOCK_SKIP_WIFI_CONNECT
    if (safe_mode) {
        ESP_LOGW(TAG, "Safe mode: skipping swarm bridge init");
    } else if (g_nvs_config.seed_url[0] != '\0') {
        swarm_config_t swarm_cfg = {
            .heartbeat_sec = g_nvs_config.swarm_heartbeat_sec,
            .ingest_sec    = g_nvs_config.swarm_ingest_sec,
            .enabled       = 1,
        };
        strncpy(swarm_cfg.seed_url, g_nvs_config.seed_url, sizeof(swarm_cfg.seed_url) - 1);
        strncpy(swarm_cfg.seed_token, g_nvs_config.seed_token, sizeof(swarm_cfg.seed_token) - 1);
        strncpy(swarm_cfg.zone_name, g_nvs_config.zone_name, sizeof(swarm_cfg.zone_name) - 1);
        swarm_cfg.seed_url[sizeof(swarm_cfg.seed_url) - 1] = '\0';
        swarm_cfg.seed_token[sizeof(swarm_cfg.seed_token) - 1] = '\0';
        swarm_cfg.zone_name[sizeof(swarm_cfg.zone_name) - 1] = '\0';
        swarm_ret = swarm_bridge_init(&swarm_cfg, g_nvs_config.node_id);
        if (swarm_ret != ESP_OK) {
            ESP_LOGW(TAG, "Swarm bridge init failed: %s", esp_err_to_name(swarm_ret));
        }
    } else {
        ESP_LOGI(TAG, "Swarm bridge disabled (no seed_url configured)");
    }
#else
    ESP_LOGI(TAG, "Mock CSI mode: skipping swarm bridge");
#endif

    /* ADR-081: I2S microphone — gated on NVS mic_enable, skipped in safe mode. */
    if (safe_mode) {
        ESP_LOGW(TAG, "Safe mode: skipping audio_mic init");
    } else if (g_nvs_config.mic_enable) {
        audio_mic_config_t mcfg = {
            .ws_gpio     = g_nvs_config.mic_ws_gpio,
            .sck_gpio    = g_nvs_config.mic_sck_gpio,
            .sd_gpio     = g_nvs_config.mic_sd_gpio,
            .sample_rate = g_nvs_config.mic_sample_rate,
            .shift_bits  = g_nvs_config.mic_shift_bits,
        };
        esp_err_t mic_ret = audio_mic_init(&mcfg);
        if (mic_ret != ESP_OK) {
            ESP_LOGW(TAG, "audio_mic init failed: %s (continuing)",
                     esp_err_to_name(mic_ret));
        }
    } else {
        ESP_LOGI(TAG, "Audio mic disabled (mic_enable=0)");
    }

    /* Initialize power management. */
    power_mgmt_init(g_nvs_config.power_duty);

    /* ADR-045: Start AMOLED display task (gracefully skips if no display).
     * Skipped in safe mode — LVGL with PSRAM frame buffers has a wider
     * crash surface than the rest of the firmware. */
#ifdef CONFIG_DISPLAY_ENABLE
    if (safe_mode) {
        ESP_LOGW(TAG, "Safe mode: skipping display task start");
    } else {
        esp_err_t disp_ret = display_task_start();
        if (disp_ret != ESP_OK) {
            ESP_LOGW(TAG, "Display init returned: %s", esp_err_to_name(disp_ret));
        }
    }
#endif

    if (ota_ret == ESP_OK) {
        esp_err_t hook_ret = ota_update_set_quiesce_hooks(ota_quiesce_runtime,
                                                          ota_restore_runtime,
                                                          NULL);
        if (hook_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to register OTA quiesce hooks: %s",
                     esp_err_to_name(hook_ret));
        }
    }

    ESP_LOGI(TAG, "CSI streaming active → %s:%d (edge_tier=%u, OTA=%s, WASM=%s, piezo=%s, mmWave=%s, swarm=%s, mode=%s)",
             g_nvs_config.target_ip, g_nvs_config.target_port,
             g_nvs_config.edge_tier,
             (ota_ret == ESP_OK) ? "ready" : "off",
             (wasm_ret == ESP_OK) ? "ready" : "off",
             (piezo_ret == ESP_OK) ? "ready" : "off",
             (mmwave_ret == ESP_OK) ? "active" : "off",
             (swarm_ret == ESP_OK) ? g_nvs_config.seed_url : "off",
             safe_mode ? "SAFE" : "normal");

    /* Arm the stability timer: if we're still alive after the configured
     * window we mark the boot as healthy and clear the persistent crash
     * counter. A panic before this fires keeps the counter elevated and
     * eventually trips safe mode. */
    const esp_timer_create_args_t stability_args = {
        .callback = stability_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "boot_stable",
    };
    esp_err_t st_create_ret = esp_timer_create(&stability_args, &s_stability_timer);
    if (st_create_ret == ESP_OK) {
        esp_err_t st_start_ret = esp_timer_start_once(
            s_stability_timer,
            (uint64_t)CONFIG_BOOT_STABILITY_DELAY_MS * 1000ULL);
        if (st_start_ret != ESP_OK) {
            ESP_LOGW(TAG, "Stability timer start failed: %s",
                     esp_err_to_name(st_start_ret));
        } else {
            ESP_LOGI(TAG, "Boot stability timer armed (%d ms)",
                     CONFIG_BOOT_STABILITY_DELAY_MS);
        }
    } else {
        ESP_LOGW(TAG, "Stability timer create failed: %s — "
                      "crash counter will not auto-clear",
                 esp_err_to_name(st_create_ret));
    }

    /* Main loop — feed the task watchdog so a wedged main task triggers a
     * panic + reboot rather than silently sitting unreachable. Feed every
     * second so the WDT timeout (default 30s) has comfortable headroom even
     * if scheduling jitter delays a wakeup. */
    while (1) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
