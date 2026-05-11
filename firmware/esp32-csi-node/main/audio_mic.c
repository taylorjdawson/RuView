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
#include "lwip/sockets.h"
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

/* Debug raw-PCM stream — ships post-HPF int16 samples to a UDP listener
 * for end-to-end audio debugging. Not a frame format with magic; just bytes. */
#define RAW_CHUNK_SAMPLES      320     /* 20 ms @ 16 kHz, 10 ms @ 32 kHz */
#define RAW_DEFAULT_DUR_S      60u
#define RAW_MAX_DUR_S          300u
#define RAW_IP_BUFLEN          16

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

    /* Raw-PCM debug stream — see audio_raw_*_handler. */
    _Atomic bool       raw_active;
    _Atomic int64_t    raw_deadline_us;
    _Atomic uint16_t   raw_target_port;
    _Atomic uint32_t   raw_chunks_sent;
    SemaphoreHandle_t  raw_ip_lock;      /* protects raw_target_ip */
    char               raw_target_ip[RAW_IP_BUFLEN];
} audio_mic_state_t;

static audio_mic_state_t s = {
    .rx_handle       = NULL,
    .task            = NULL,
    .status_lock     = NULL,
    .test_lock       = NULL,
    .test_done       = NULL,
    .test_pending    = false,
    .raw_active      = false,
    .raw_deadline_us = 0,
    .raw_target_port = 0,
    .raw_chunks_sent = 0,
    .raw_ip_lock     = NULL,
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

/* ---- Raw-PCM debug stream -------------------------------------------- */

static void emit_raw_chunk(const int16_t *chunk, size_t bytes)
{
    char ip[RAW_IP_BUFLEN];
    if (s.raw_ip_lock == NULL) return;
    if (xSemaphoreTake(s.raw_ip_lock, 0) != pdTRUE) {
        return;  /* lock held by /start handler — drop this 10/20 ms chunk */
    }
    memcpy(ip, s.raw_target_ip, sizeof(ip));
    xSemaphoreGive(s.raw_ip_lock);

    if (ip[0] == '\0') return;
    uint16_t port = atomic_load(&s.raw_target_port);
    if (port == 0) return;

    int sent = stream_sender_send_to((const uint8_t *)chunk, bytes, ip, port);
    if (sent > 0) {
        atomic_fetch_add(&s.raw_chunks_sent, 1u);
    }
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

    /* Raw-stream chunk buffer (lives on the audio task stack via this scope). */
    int16_t  raw_chunk[RAW_CHUNK_SAMPLES];
    size_t   raw_fill = 0;
    bool     raw_was_active = false;

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

            /* Raw-stream mode check, evaluated per-sample so we can transition
             * mid-DMA-buffer when the deadline elapses. */
            bool raw_now = atomic_load(&s.raw_active);
            if (raw_now && esp_timer_get_time() >= atomic_load(&s.raw_deadline_us)) {
                atomic_store(&s.raw_active, false);
                ESP_LOGI(TAG, "raw stream deadline elapsed — back to features");
                raw_now = false;
            }

            if (raw_now) {
                /* Apply the same HPF as the feature path (shared IIR state in
                 * win.x_prev/y_prev keeps continuity across mode switches). */
                float dy = (float)win.y_prev + (float)(aligned - win.x_prev);
                int32_t y = (int32_t)lrintf(s.hpf_alpha * dy);
                win.x_prev = aligned;
                win.y_prev = y;

                /* Saturate int32 → int16. */
                if (y > 32767) y = 32767;
                else if (y < -32768) y = -32768;
                raw_chunk[raw_fill++] = (int16_t)y;

                if (raw_fill >= RAW_CHUNK_SAMPLES) {
                    emit_raw_chunk(raw_chunk, raw_fill * sizeof(int16_t));
                    raw_fill = 0;
                }
                raw_was_active = true;
                continue;  /* skip feature accumulation while raw is active */
            }

            if (raw_was_active) {
                /* Just exited raw mode — discard partial chunk and reset the
                 * feature window so the next emission has clean accumulators. */
                raw_fill = 0;
                window_reset(&win);
                raw_was_active = false;
            }

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
    s.raw_ip_lock = xSemaphoreCreateMutex();
    if (s.status_lock == NULL || s.test_lock == NULL ||
        s.test_done == NULL   || s.raw_ip_lock == NULL) {
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

esp_err_t audio_mic_pause(void)
{
    if (s.task == NULL) {
        return ESP_OK;  /* mic was never initialized — no-op */
    }
    vTaskSuspend(s.task);
    if (s.rx_handle != NULL) {
        esp_err_t err = i2s_channel_disable(s.rx_handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s_channel_disable on pause: %s", esp_err_to_name(err));
            /* Resume the task so we don't end up wedged. */
            vTaskResume(s.task);
            return err;
        }
    }
    ESP_LOGI(TAG, "audio mic paused");
    return ESP_OK;
}

esp_err_t audio_mic_resume(void)
{
    if (s.task == NULL) {
        return ESP_OK;
    }
    if (s.rx_handle != NULL) {
        esp_err_t err = i2s_channel_enable(s.rx_handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s_channel_enable on resume: %s", esp_err_to_name(err));
            return err;
        }
    }
    vTaskResume(s.task);
    ESP_LOGI(TAG, "audio mic resumed");
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

/* ---- /audio/raw_stream handlers -------------------------------------- */

/* Pull the IPv4 dotted-quad of the HTTP peer out of the underlying socket.
 * Used as the default destination for raw_stream sends so the CLI can simply
 * POST without first having to know its own LAN IP. */
static esp_err_t get_peer_ipv4(httpd_req_t *req, char *out, size_t out_len)
{
    if (req == NULL || out == NULL || out_len < INET_ADDRSTRLEN) {
        return ESP_ERR_INVALID_ARG;
    }
    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd < 0) return ESP_FAIL;

    struct sockaddr_in6 peer;
    socklen_t peer_len = sizeof(peer);
    if (getpeername(sockfd, (struct sockaddr *)&peer, &peer_len) != 0) {
        return ESP_FAIL;
    }

    if (peer.sin6_family == AF_INET) {
        const struct sockaddr_in *p4 = (const struct sockaddr_in *)&peer;
        if (inet_ntop(AF_INET, &p4->sin_addr, out, out_len) == NULL) {
            return ESP_FAIL;
        }
        return ESP_OK;
    }
    if (peer.sin6_family == AF_INET6 && IN6_IS_ADDR_V4MAPPED(&peer.sin6_addr)) {
        /* IPv4-mapped IPv6: extract last 4 bytes. */
        if (inet_ntop(AF_INET, &peer.sin6_addr.s6_addr[12], out, out_len) == NULL) {
            return ESP_FAIL;
        }
        return ESP_OK;
    }
    return ESP_FAIL;
}

static uint32_t parse_query_u32(const char *query, const char *key, uint32_t fallback)
{
    char val[16] = {0};
    if (httpd_query_key_value(query, key, val, sizeof(val)) != ESP_OK) {
        return fallback;
    }
    char *end = NULL;
    unsigned long v = strtoul(val, &end, 10);
    if (end == val) return fallback;
    return (uint32_t)v;
}

static esp_err_t audio_raw_start_handler(httpd_req_t *req)
{
    if (ota_update_require_auth(req, "audio raw_stream start") != ESP_OK) {
        return ESP_OK;  /* require_auth already sent the 401 */
    }

    char query[160] = {0};
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0 && qlen < sizeof(query)) {
        httpd_req_get_url_query_str(req, query, sizeof(query));
    }

    uint32_t duration_s = parse_query_u32(query, "duration_s", RAW_DEFAULT_DUR_S);
    uint32_t port       = parse_query_u32(query, "port",       0u);

    if (duration_s < 1u) duration_s = 1u;
    if (duration_s > RAW_MAX_DUR_S) duration_s = RAW_MAX_DUR_S;
    if (port < 1024u || port > 65535u) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "port must be in [1024, 65535]");
    }

    char ip[RAW_IP_BUFLEN] = {0};
    if (httpd_query_key_value(query, "ip", ip, sizeof(ip)) == ESP_OK && ip[0] != '\0') {
        struct in_addr tmp;
        if (inet_pton(AF_INET, ip, &tmp) <= 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "invalid ip (expected IPv4 dotted-quad)");
        }
    } else {
        if (get_peer_ipv4(req, ip, sizeof(ip)) != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "could not resolve peer IP; pass ?ip=");
        }
    }

    /* Update destination atomically (port + ip behind lock + flag last). */
    if (s.raw_ip_lock != NULL && xSemaphoreTake(s.raw_ip_lock, portMAX_DELAY) == pdTRUE) {
        memset(s.raw_target_ip, 0, sizeof(s.raw_target_ip));
        strncpy(s.raw_target_ip, ip, sizeof(s.raw_target_ip) - 1);
        xSemaphoreGive(s.raw_ip_lock);
    }
    atomic_store(&s.raw_target_port, (uint16_t)port);
    atomic_store(&s.raw_chunks_sent, 0u);
    int64_t deadline = esp_timer_get_time() + (int64_t)duration_s * 1000000LL;
    atomic_store(&s.raw_deadline_us, deadline);
    atomic_store(&s.raw_active, true);

    char resp[160];
    int n = snprintf(resp, sizeof(resp),
        "{\"ok\":true,\"duration_s\":%lu,\"ip\":\"%s\",\"port\":%lu}",
        (unsigned long)duration_s, ip, (unsigned long)port);
    if (n < 0 || n >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "raw_stream start serialization");
    }
    ESP_LOGI(TAG, "raw_stream start → %s:%lu for %lus",
             ip, (unsigned long)port, (unsigned long)duration_s);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t audio_raw_stop_handler(httpd_req_t *req)
{
    if (ota_update_require_auth(req, "audio raw_stream stop") != ESP_OK) {
        return ESP_OK;
    }
    bool was = atomic_exchange(&s.raw_active, false);
    ESP_LOGI(TAG, "raw_stream stop (was_active=%s)", was ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t audio_raw_status_handler(httpd_req_t *req)
{
    bool active = atomic_load(&s.raw_active);
    int64_t deadline = atomic_load(&s.raw_deadline_us);
    int64_t now_us = esp_timer_get_time();
    int64_t remaining_ms = active ? (deadline - now_us) / 1000 : 0;
    if (remaining_ms < 0) remaining_ms = 0;
    uint16_t port = atomic_load(&s.raw_target_port);
    uint32_t chunks = atomic_load(&s.raw_chunks_sent);

    char ip[RAW_IP_BUFLEN] = {0};
    if (s.raw_ip_lock != NULL && xSemaphoreTake(s.raw_ip_lock, 0) == pdTRUE) {
        memcpy(ip, s.raw_target_ip, sizeof(ip));
        xSemaphoreGive(s.raw_ip_lock);
    }

    char resp[200];
    int n = snprintf(resp, sizeof(resp),
        "{\"active\":%s,\"remaining_ms\":%lld,\"ip\":\"%s\","
        "\"port\":%u,\"chunks_sent\":%lu,\"sample_rate\":%u,"
        "\"chunk_samples\":%u}",
        active ? "true" : "false",
        (long long)remaining_ms, ip, (unsigned)port,
        (unsigned long)chunks, (unsigned)s.cfg.sample_rate,
        (unsigned)RAW_CHUNK_SAMPLES);
    if (n < 0 || n >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "raw_stream status serialization");
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
    httpd_uri_t raw_start_uri = {
        .uri = "/audio/raw_stream/start", .method = HTTP_POST,
        .handler = audio_raw_start_handler, .user_ctx = NULL,
    };
    httpd_uri_t raw_stop_uri = {
        .uri = "/audio/raw_stream/stop", .method = HTTP_POST,
        .handler = audio_raw_stop_handler, .user_ctx = NULL,
    };
    httpd_uri_t raw_status_uri = {
        .uri = "/audio/raw_stream/status", .method = HTTP_GET,
        .handler = audio_raw_status_handler, .user_ctx = NULL,
    };

    ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(server, &status_uri));
    ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(server, &test_uri));
    ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(server, &raw_start_uri));
    ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(server, &raw_stop_uri));
    ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(server, &raw_status_uri));

    ESP_LOGI(TAG, "Audio HTTP control ready:");
    ESP_LOGI(TAG, "  GET  /audio/status");
    ESP_LOGI(TAG, "  POST /audio/test");
    ESP_LOGI(TAG, "  POST /audio/raw_stream/start?duration_s=N&port=P[&ip=A]");
    ESP_LOGI(TAG, "  POST /audio/raw_stream/stop");
    ESP_LOGI(TAG, "  GET  /audio/raw_stream/status");
    return ESP_OK;
}
