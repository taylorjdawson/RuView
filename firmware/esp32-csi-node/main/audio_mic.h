/**
 * @file audio_mic.h
 * @brief ADR-081: I2S MEMS microphone driver (SPH0645LM4H-B compatible).
 *
 * Captures 32-bit stereo I2S samples (left channel only is meaningful when
 * the mic's L/R pin is tied to GND), shifts/sign-extends to ~24-bit signed,
 * applies a first-order high-pass filter to remove the SPH0645's DC bias,
 * and emits a one-second window of features (RMS, peak) as a UDP frame
 * with magic 0xC5110007. Mirrors the on-device feature pattern used by
 * edge_processing.c — the server stays out of the per-sample audio loop.
 *
 * --- Pin layout (LOCKED — canonical source is ADR-081) ---
 *   Piezo  D0 / GPIO 1   (LEDC PWM, existing)
 *   Mic WS D1 / GPIO 2   (I2S WS / LRCLK)
 *   skip   D2 / GPIO 3   (ESP32-S3 strap pin)
 *   Mic SCK D3 / GPIO 4  (I2S BCLK)
 *   Mic SD  D4 / GPIO 5  (I2S DIN)
 *   Mic VDD 3V3, GND GND, L/R bridged to GND for left-channel only.
 *
 * Init is gated by NVS `mic_enable` (default 0). Same firmware runs on
 * speaker-only, mic-only, both, and neither nodes — no recompile needed.
 */

#ifndef AUDIO_MIC_H
#define AUDIO_MIC_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

typedef struct {
    uint8_t  ws_gpio;
    uint8_t  sck_gpio;
    uint8_t  sd_gpio;
    uint16_t sample_rate;   /**< 16000 nominal */
    uint8_t  shift_bits;    /**< 32->24 alignment, default 14 */
} audio_mic_config_t;

typedef struct {
    bool     initialized;
    uint16_t sample_rate;
    uint32_t frames_emitted;   /**< 1 s windows shipped over UDP */
    float    last_rms;         /**< most recent post-HPF RMS, normalized -1..1 */
    float    last_peak;        /**< most recent post-HPF peak abs */
    int64_t  last_dc_pre;      /**< pre-HPF mean of int32 samples */
    int32_t  last_dc_post;     /**< post-HPF mean (should be near 0) */
} audio_mic_status_t;

typedef struct {
    float   rms;
    float   peak;
    int32_t min_sample;
    int32_t max_sample;
    float   mean_post_hpf;
} audio_mic_test_result_t;

/**
 * Initialize the I2S audio capture pipeline.
 *
 * Spawns a background task on core 1 that pulls DMA buffers from the I2S
 * peripheral, runs the DC-removal HPF, accumulates per-window stats, and
 * emits an audio frame (magic 0xC5110007) over UDP every sample_rate samples.
 *
 * @return ESP_OK on success. ESP_ERR_INVALID_ARG if cfg is bad.
 */
esp_err_t audio_mic_init(const audio_mic_config_t *cfg);

/** Thread-safe snapshot of the latest feature window. */
void audio_mic_get_status(audio_mic_status_t *out);

/**
 * Synchronous 1-second capture for diagnostics. Returns aggregated stats
 * over the just-captured window. Useful for sweeping shift_bits without
 * waiting for the periodic UDP frame.
 *
 * @return ESP_OK if the mic is initialized and the capture completes.
 */
esp_err_t audio_mic_run_test(audio_mic_test_result_t *out);

/** Register /audio/status and /audio/test endpoints on the OTA HTTP server. */
esp_err_t audio_mic_register_http(httpd_handle_t server);

/**
 * Pause the audio capture task and disable the I2S channel. Safe to call when
 * audio_mic was never initialized (mic_enable=0 nodes return ESP_OK no-op).
 *
 * Used by the OTA quiesce hook so the WiFi/HTTP stack has full CPU + buffer
 * headroom while a 1 MiB firmware blob streams in. Without this, OTA POST
 * hangs indefinitely on any node with mic_enable=1.
 *
 * @return ESP_OK on success or no-op; ESP_FAIL if I2S disable failed.
 */
esp_err_t audio_mic_pause(void);

/** Re-enable the I2S channel and resume the audio task. Pair with audio_mic_pause. */
esp_err_t audio_mic_resume(void);

#endif /* AUDIO_MIC_H */
