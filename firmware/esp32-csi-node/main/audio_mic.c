/**
 * @file audio_mic.c
 * @brief ADR-081 I2S audio mic driver — see audio_mic.h.
 */

#include "audio_mic.h"

#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_config.h"
#include "ota_update.h"
#include "stream_sender.h"

static const char *TAG = "audio_mic";

#define AUDIO_FRAME_MAGIC      0xC5110007u
#define AUDIO_FRAME_VERSION    0x01u
#define AUDIO_HEADER_BYTES     26
#define DMA_FRAMES             8
#define DMA_FRAME_SAMPLES      320     /* 20 ms @ 16 kHz */
#define STARTUP_DISCARD_MS     50
#define HPF_DEFAULT_CUTOFF_HZ  80.0f
#define WINDOW_LOG_SECONDS     1
#define TEST_CAPTURE_MS        1000

extern nvs_config_t g_nvs_config;

typedef struct {
    audio_mic_config_t cfg;
    i2s_chan_handle_t  rx_handle;
    TaskHandle_t       task;
    SemaphoreHandle_t  status_lock;
    audio_mic_status_t status;
    SemaphoreHandle_t  test_lock;        /* serializes audio_mic_run_test */
    audio_mic_test_result_t test_result;
    SemaphoreHandle_t  test_done;
    _Atomic bool       test_pending;     /* task picks this up between windows */
    float              hpf_alpha;        /* IIR HPF coefficient */
} audio_mic_state_t;

static audio_mic_state_t s = {
    .rx_handle    = NULL,
    .task         = NULL,
    .status_lock  = NULL,
    .test_lock    = NULL,
    .test_done    = NULL,
    .test_pending = false,
};

/* ---- Helpers ---------------------------------------------------------- */

static inline int32_t shift_align(int32_t raw, uint8_t shift_bits)
{
    /* Arithmetic right-shift on a signed int32 sign-extends by ESP32-S3 ABI. */
    return raw >> shift_bits;
}

static esp_err_t install_i2s(const audio_mic_config_t *cfg)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = DMA_FRAMES;
    chan_cfg.dma_frame_num = DMA_FRAME_SAMPLES;
    chan_cfg.auto_clear    = true;

    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s.rx_handle),
                        TAG, "i2s_new_channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(cfg->sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)cfg->sck_gpio,
            .ws   = (gpio_num_t)cfg->ws_gpio,
            .dout = I2S_GPIO_UNUSED,
            .din  = (gpio_num_t)cfg->sd_gpio,
            .invert_flags = { 0 },
        },
    };

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s.rx_handle, &std_cfg),
                        TAG, "i2s_channel_init_std_mode failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s.rx_handle),
                        TAG, "i2s_channel_enable failed");

    ESP_LOGI(TAG, "I2S RX configured: bclk=GPIO%u ws=GPIO%u din=GPIO%u sr=%u slot=stereo32",
             (unsigned)cfg->sck_gpio, (unsigned)cfg->ws_gpio,
             (unsigned)cfg->sd_gpio, (unsigned)cfg->sample_rate);
    return ESP_OK;
}

static void compute_hpf_alpha(audio_mic_state_t *st, float fc_hz, float fs_hz)
{
    /* First-order IIR HPF: y[n] = α(y[n-1] + x[n] - x[n-1])
     * α = exp(-2π fc / fs) is a standard approximation; near 1 for fc << fs. */
    float a = expf(-2.0f * (float)M_PI * fc_hz / fs_hz);
    if (a < 0.0f) a = 0.0f;
    if (a > 0.999f) a = 0.999f;
    st->hpf_alpha = a;
}

/* ---- Window stats ----------------------------------------------------- */

typedef struct {
    uint32_t samples;
    int64_t  dc_pre_acc;       /* pre-HPF accumulator (int32 samples) */
    int64_t  dc_post_acc;      /* post-HPF accumulator (int32 samples) */
    double   sum_sq;           /* RMS² accumulator (in int32 sample² space) */
    int32_t  peak_abs;
    int32_t  min_sample;
    int32_t  max_sample;
    /* IIR state */
    int32_t  x_prev;
    int32_t  y_prev;
} window_acc_t;

static void window_reset(window_acc_t *w)
{
    w->samples = 0;
    w->dc_pre_acc = 0;
    w->dc_post_acc = 0;
    w->sum_sq = 0.0;
    w->peak_abs = 0;
    w->min_sample = INT32_MAX;
    w->max_sample = INT32_MIN;
    w->x_prev = 0;
    w->y_prev = 0;
}

static inline void window_consume(window_acc_t *w, int32_t aligned, float alpha)
{
    /* Filter on integer samples to avoid float per-sample. */
    /* y[n] = round( α * (y_prev + (aligned - x_prev)) )  */
    float dy = (float)w->y_prev + (float)(aligned - w->x_prev);
    int32_t y = (int32_t)lrintf(alpha * dy);
    w->x_prev = aligned;
    w->y_prev = y;

    w->dc_pre_acc  += aligned;
    w->dc_post_acc += y;
    w->sum_sq      += (double)y * (double)y;

    int32_t a = y < 0 ? -y : y;
    if (a > w->peak_abs) w->peak_abs = a;
    if (aligned < w->min_sample) w->min_sample = aligned;
    if (aligned > w->max_sample) w->max_sample = aligned;
    w->samples++;
}

/* Convert an int32 sample (24-bit-aligned) to normalized -1..1 float. */
static inline float to_unit(int32_t s24)
{
    /* 24-bit signed range = [-2^23, 2^23-1] */
    return (float)s24 / 8388608.0f;
}

static void window_finalize_and_publish(window_acc_t *w, uint32_t window_seq)
{
    if (w->samples == 0) return;

    double mean_sq = w->sum_sq / (double)w->samples;
    float  rms_int = (float)sqrt(mean_sq);
    float  rms_unit  = to_unit((int32_t)lrintf(rms_int));
    float  peak_unit = to_unit(w->peak_abs);
    int64_t dc_pre   = w->dc_pre_acc / (int64_t)w->samples;
    int32_t dc_post  = (int32_t)(w->dc_post_acc / (int64_t)w->samples);

    /* Update shared status */
    if (xSemaphoreTake(s.status_lock, portMAX_DELAY) == pdTRUE) {
        s.status.last_rms     = rms_unit;
        s.status.last_peak    = peak_unit;
        s.status.last_dc_pre  = dc_pre;
        s.status.last_dc_post = dc_post;
        s.status.frames_emitted++;
        xSemaphoreGive(s.status_lock);
    }

    ESP_LOGI(TAG,
             "sr=%u win=%ums rms=%.4f peak=%.4f dc_pre=%lld dc_post=%ld samples=%lu",
             (unsigned)s.cfg.sample_rate,
             (unsigned)WINDOW_LOG_SECONDS * 1000u,
             (double)rms_unit, (double)peak_unit,
             (long long)dc_pre, (long)dc_post,
             (unsigned long)w->samples);

    /* If a synchronous test capture is pending, hand off the result. */
    if (atomic_load(&s.test_pending)) {
        s.test_result.rms = rms_unit;
        s.test_result.peak = peak_unit;
        s.test_result.min_sample = w->min_sample;
        s.test_result.max_sample = w->max_sample;
        s.test_result.mean_post_hpf = (float)dc_post;
        atomic_store(&s.test_pending, false);
        xSemaphoreGive(s.test_done);
    }

    /* UDP frame: 26-byte header + RMS/peak floats. */
    uint8_t pkt[AUDIO_HEADER_BYTES];
    uint64_t now_us = (uint64_t)esp_timer_get_time();

    pkt[0] = (uint8_t)(AUDIO_FRAME_MAGIC & 0xFF);
    pkt[1] = (uint8_t)((AUDIO_FRAME_MAGIC >> 8) & 0xFF);
    pkt[2] = (uint8_t)((AUDIO_FRAME_MAGIC >> 16) & 0xFF);
    pkt[3] = (uint8_t)((AUDIO_FRAME_MAGIC >> 24) & 0xFF);
    pkt[4] = g_nvs_config.node_id;
    pkt[5] = AUDIO_FRAME_VERSION;
    pkt[6] = (uint8_t)(s.cfg.sample_rate & 0xFF);
    pkt[7] = (uint8_t)((s.cfg.sample_rate >> 8) & 0xFF);
    /* window_seq u32 LE */
    pkt[8]  = (uint8_t)(window_seq & 0xFF);
    pkt[9]  = (uint8_t)((window_seq >> 8) & 0xFF);
    pkt[10] = (uint8_t)((window_seq >> 16) & 0xFF);
    pkt[11] = (uint8_t)((window_seq >> 24) & 0xFF);
    /* window_start_us u32 LE (low 32 bits) */
    uint32_t start_lo = (uint32_t)(now_us & 0xFFFFFFFFu);
    pkt[12] = (uint8_t)(start_lo & 0xFF);
    pkt[13] = (uint8_t)((start_lo >> 8) & 0xFF);
    pkt[14] = (uint8_t)((start_lo >> 16) & 0xFF);
    pkt[15] = (uint8_t)((start_lo >> 24) & 0xFF);
    /* window_samples u16 LE */
    uint16_t ws = (w->samples > 0xFFFFu) ? 0xFFFFu : (uint16_t)w->samples;
    pkt[16] = (uint8_t)(ws & 0xFF);
    pkt[17] = (uint8_t)((ws >> 8) & 0xFF);
    /* RMS float32 LE + peak float32 LE */
    memcpy(&pkt[18], &rms_unit, 4);
    memcpy(&pkt[22], &peak_unit, 4);

    stream_sender_send(pkt, sizeof(pkt));
}

/* ---- Background task -------------------------------------------------- */

static void audio_task(void *arg)
{
    (void)arg;
    int32_t *dma_buf = (int32_t *)heap_caps_malloc(
        DMA_FRAME_SAMPLES * 2 * sizeof(int32_t),
        MALLOC_CAP_DEFAULT);
    if (dma_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate DMA scratch buffer");
        vTaskDelete(NULL);
        return;
    }

    /* Discard startup samples — handoff §3 says mic output invalid for ~50 ms */
    {
        uint32_t startup_samples = (uint32_t)s.cfg.sample_rate * STARTUP_DISCARD_MS / 1000u;
        ESP_LOGI(TAG, "discarding %ums startup samples (~%lu samples)",
                 (unsigned)STARTUP_DISCARD_MS, (unsigned long)startup_samples);
        size_t bytes_read = 0;
        uint32_t consumed = 0;
        while (consumed < startup_samples) {
            esp_err_t err = i2s_channel_read(s.rx_handle, dma_buf,
                                             sizeof(int32_t) * 2 * DMA_FRAME_SAMPLES,
                                             &bytes_read, pdMS_TO_TICKS(200));
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "i2s_channel_read (startup): %s", esp_err_to_name(err));
                break;
            }
            consumed += bytes_read / (sizeof(int32_t) * 2);
        }
    }

    if (xSemaphoreTake(s.status_lock, portMAX_DELAY) == pdTRUE) {
        s.status.initialized = true;
        s.status.sample_rate = s.cfg.sample_rate;
        xSemaphoreGive(s.status_lock);
    }
    ESP_LOGI(TAG, "bringup task running on core %d", xPortGetCoreID());

    window_acc_t win;
    window_reset(&win);

    uint32_t window_target = s.cfg.sample_rate * WINDOW_LOG_SECONDS;
    uint32_t window_seq    = 0;

    for (;;) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s.rx_handle, dma_buf,
                                         sizeof(int32_t) * 2 * DMA_FRAME_SAMPLES,
                                         &bytes_read, pdMS_TO_TICKS(500));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s_channel_read: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* Stereo frames — left=index 0, right=index 1. We use only left. */
        size_t pairs = bytes_read / (sizeof(int32_t) * 2);
        for (size_t i = 0; i < pairs; i++) {
            int32_t left_raw = dma_buf[2 * i];
            int32_t aligned  = shift_align(left_raw, s.cfg.shift_bits);
            window_consume(&win, aligned, s.hpf_alpha);

            if (win.samples >= window_target) {
                window_finalize_and_publish(&win, window_seq++);
                window_reset(&win);
            }
        }
    }
}

/* ---- Public API ------------------------------------------------------- */

esp_err_t audio_mic_init(const audio_mic_config_t *cfg)
{
    if (cfg == NULL || cfg->sample_rate == 0 || cfg->shift_bits == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s.task != NULL) {
        ESP_LOGW(TAG, "audio_mic_init called twice — ignoring");
        return ESP_OK;
    }

    s.cfg = *cfg;
    s.status_lock = xSemaphoreCreateMutex();
    s.test_lock   = xSemaphoreCreateMutex();
    s.test_done   = xSemaphoreCreateBinary();
    if (s.status_lock == NULL || s.test_lock == NULL || s.test_done == NULL) {
        ESP_LOGE(TAG, "Failed to create sync primitives");
        return ESP_ERR_NO_MEM;
    }

    compute_hpf_alpha(&s, HPF_DEFAULT_CUTOFF_HZ, (float)cfg->sample_rate);
    ESP_LOGI(TAG, "HPF α=%.5f (cutoff=%.0f Hz, fs=%u Hz)",
             (double)s.hpf_alpha, (double)HPF_DEFAULT_CUTOFF_HZ,
             (unsigned)cfg->sample_rate);

    esp_err_t err = install_i2s(cfg);
    if (err != ESP_OK) return err;

    BaseType_t ok = xTaskCreatePinnedToCore(audio_task, "audio_mic", 4096,
                                            NULL, 5, &s.task, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn audio_mic task");
        i2s_channel_disable(s.rx_handle);
        i2s_del_channel(s.rx_handle);
        s.rx_handle = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void audio_mic_get_status(audio_mic_status_t *out)
{
    if (out == NULL) return;
    if (s.status_lock != NULL && xSemaphoreTake(s.status_lock, portMAX_DELAY) == pdTRUE) {
        *out = s.status;
        xSemaphoreGive(s.status_lock);
    } else {
        memset(out, 0, sizeof(*out));
    }
}

esp_err_t audio_mic_run_test(audio_mic_test_result_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (s.task == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s.test_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    /* Drain any stale signal. */
    xSemaphoreTake(s.test_done, 0);

    atomic_store(&s.test_pending, true);
    BaseType_t got = xSemaphoreTake(s.test_done,
                                    pdMS_TO_TICKS(TEST_CAPTURE_MS * 3));
    if (got == pdTRUE) {
        *out = s.test_result;
        xSemaphoreGive(s.test_lock);
        return ESP_OK;
    }
    atomic_store(&s.test_pending, false);
    xSemaphoreGive(s.test_lock);
    return ESP_ERR_TIMEOUT;
}

/* ---- HTTP endpoints --------------------------------------------------- */

static esp_err_t audio_status_handler(httpd_req_t *req)
{
    audio_mic_status_t st;
    audio_mic_get_status(&st);

    char resp[256];
    int n = snprintf(resp, sizeof(resp),
        "{\"initialized\":%s,\"sample_rate\":%u,\"frames_emitted\":%lu,"
        "\"last_rms\":%.6f,\"last_peak\":%.6f,"
        "\"last_dc_pre\":%lld,\"last_dc_post\":%ld}",
        st.initialized ? "true" : "false",
        (unsigned)st.sample_rate,
        (unsigned long)st.frames_emitted,
        (double)st.last_rms, (double)st.last_peak,
        (long long)st.last_dc_pre, (long)st.last_dc_post);
    if (n < 0 || n >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "audio status serialization");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t audio_test_handler(httpd_req_t *req)
{
    if (ota_update_require_auth(req, "audio test") != ESP_OK) {
        return ESP_OK;
    }

    audio_mic_test_result_t r;
    esp_err_t err = audio_mic_run_test(&r);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   esp_err_to_name(err));
    }

    char resp[256];
    int n = snprintf(resp, sizeof(resp),
        "{\"rms\":%.6f,\"peak\":%.6f,\"min\":%ld,\"max\":%ld,"
        "\"mean_post_hpf\":%.3f}",
        (double)r.rms, (double)r.peak,
        (long)r.min_sample, (long)r.max_sample,
        (double)r.mean_post_hpf);
    if (n < 0 || n >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "audio test serialization");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

esp_err_t audio_mic_register_http(httpd_handle_t server)
{
    if (server == NULL) return ESP_ERR_INVALID_ARG;

    httpd_uri_t status_uri = {
        .uri = "/audio/status", .method = HTTP_GET,
        .handler = audio_status_handler, .user_ctx = NULL,
    };
    httpd_uri_t test_uri = {
        .uri = "/audio/test", .method = HTTP_POST,
        .handler = audio_test_handler, .user_ctx = NULL,
    };

    ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(server, &status_uri));
    ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(server, &test_uri));

    ESP_LOGI(TAG, "Audio HTTP control ready:");
    ESP_LOGI(TAG, "  GET  /audio/status");
    ESP_LOGI(TAG, "  POST /audio/test");
    return ESP_OK;
}
