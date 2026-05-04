/**
 * @file esp_stubs.c
 * @brief Implementation of ESP-IDF stubs for host-based fuzz testing.
 *
 * Must be compiled with: -Istubs -I../main
 * so that ESP-IDF headers resolve to stubs/ and firmware headers
 * resolve to ../main/.
 */

#include "esp_stubs.h"
#include "edge_processing.h"
#include "wasm_runtime.h"
#include <stdint.h>
#include <string.h>

/* ---- esp_system / reset reason stub ---- */

static esp_reset_reason_t s_reset_reason = ESP_RST_POWERON;

esp_reset_reason_t esp_reset_reason(void)
{
    return s_reset_reason;
}

void _test_set_reset_reason(esp_reset_reason_t r)
{
    s_reset_reason = r;
}

/* ---- in-memory NVS store ---- */

#define STUB_NVS_MAX_ENTRIES 32
#define STUB_NVS_KEY_LEN     16
#define STUB_NVS_NS_LEN      16

typedef enum {
    NVS_STUB_TYPE_NONE = 0,
    NVS_STUB_TYPE_U8,
} nvs_stub_value_type_t;

typedef struct {
    bool                  in_use;
    char                  ns[STUB_NVS_NS_LEN];
    char                  key[STUB_NVS_KEY_LEN];
    nvs_stub_value_type_t type;
    uint8_t               u8_val;
} nvs_stub_entry_t;

static nvs_stub_entry_t s_nvs_table[STUB_NVS_MAX_ENTRIES];
/* nvs_handle_t is currently just an index into s_nvs_namespaces[]. */
static char s_nvs_namespaces[STUB_NVS_MAX_ENTRIES][STUB_NVS_NS_LEN];
static uint32_t s_next_handle = 1;  /* 0 is reserved as "invalid" */

void _test_nvs_clear(void)
{
    memset(s_nvs_table, 0, sizeof(s_nvs_table));
    memset(s_nvs_namespaces, 0, sizeof(s_nvs_namespaces));
    s_next_handle = 1;
}

static const char *handle_namespace(nvs_handle_t h)
{
    if (h == 0 || h >= STUB_NVS_MAX_ENTRIES) return NULL;
    return s_nvs_namespaces[h];
}

static nvs_stub_entry_t *find_entry(const char *ns, const char *key, bool create)
{
    for (int i = 0; i < STUB_NVS_MAX_ENTRIES; i++) {
        if (s_nvs_table[i].in_use
            && strcmp(s_nvs_table[i].ns, ns) == 0
            && strcmp(s_nvs_table[i].key, key) == 0) {
            return &s_nvs_table[i];
        }
    }
    if (!create) return NULL;
    for (int i = 0; i < STUB_NVS_MAX_ENTRIES; i++) {
        if (!s_nvs_table[i].in_use) {
            s_nvs_table[i].in_use = true;
            strncpy(s_nvs_table[i].ns, ns, STUB_NVS_NS_LEN - 1);
            s_nvs_table[i].ns[STUB_NVS_NS_LEN - 1] = '\0';
            strncpy(s_nvs_table[i].key, key, STUB_NVS_KEY_LEN - 1);
            s_nvs_table[i].key[STUB_NVS_KEY_LEN - 1] = '\0';
            return &s_nvs_table[i];
        }
    }
    return NULL;
}

esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h)
{
    (void)mode;
    if (ns == NULL || h == NULL) return ESP_ERR_INVALID_ARG;
    if (s_next_handle >= STUB_NVS_MAX_ENTRIES) return ESP_FAIL;
    nvs_handle_t handle = s_next_handle++;
    strncpy(s_nvs_namespaces[handle], ns, STUB_NVS_NS_LEN - 1);
    s_nvs_namespaces[handle][STUB_NVS_NS_LEN - 1] = '\0';
    *h = handle;
    return ESP_OK;
}

void nvs_close(nvs_handle_t h)
{
    (void)h;  /* No-op — entries persist for test stability. */
}

esp_err_t nvs_commit(nvs_handle_t h)
{
    (void)h;
    return ESP_OK;
}

esp_err_t nvs_get_u8(nvs_handle_t h, const char *k, uint8_t *v)
{
    const char *ns = handle_namespace(h);
    if (ns == NULL || k == NULL || v == NULL) return ESP_ERR_INVALID_ARG;
    nvs_stub_entry_t *e = find_entry(ns, k, false);
    if (e == NULL || e->type != NVS_STUB_TYPE_U8) return ESP_ERR_NOT_FOUND;
    *v = e->u8_val;
    return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t h, const char *k, uint8_t v)
{
    const char *ns = handle_namespace(h);
    if (ns == NULL || k == NULL) return ESP_ERR_INVALID_ARG;
    nvs_stub_entry_t *e = find_entry(ns, k, true);
    if (e == NULL) return ESP_FAIL;
    e->type = NVS_STUB_TYPE_U8;
    e->u8_val = v;
    return ESP_OK;
}

/* The remaining read APIs aren't exercised by the boot_health tests; return
 * ESP_ERR_NOT_FOUND so callers see "no override stored". */
esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *v, size_t *l)
{ (void)h; (void)k; (void)v; (void)l; return ESP_ERR_NOT_FOUND; }
esp_err_t nvs_get_u16(nvs_handle_t h, const char *k, uint16_t *v)
{ (void)h; (void)k; (void)v; return ESP_ERR_NOT_FOUND; }
esp_err_t nvs_get_u32(nvs_handle_t h, const char *k, uint32_t *v)
{ (void)h; (void)k; (void)v; return ESP_ERR_NOT_FOUND; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *v, size_t *l)
{ (void)h; (void)k; (void)v; (void)l; return ESP_ERR_NOT_FOUND; }

/** Monotonically increasing microsecond counter for esp_timer_get_time(). */
static int64_t s_fake_time_us = 0;

int64_t esp_timer_get_time(void)
{
    /* Advance by 50ms each call (~20 Hz CSI rate simulation). */
    s_fake_time_us += 50000;
    return s_fake_time_us;
}

/* ---- stream_sender stubs ---- */

int stream_sender_send(const uint8_t *data, size_t len)
{
    (void)data;
    return (int)len;
}

int stream_sender_init(void)
{
    return 0;
}

int stream_sender_init_with(const char *ip, uint16_t port)
{
    (void)ip; (void)port;
    return 0;
}

void stream_sender_deinit(void)
{
}

/* ---- wasm_runtime stubs ---- */

void wasm_runtime_on_frame(const float *phases, const float *amplitudes,
                           const float *variances, uint16_t n_sc,
                           const edge_vitals_pkt_t *vitals)
{
    (void)phases; (void)amplitudes; (void)variances;
    (void)n_sc; (void)vitals;
}

esp_err_t wasm_runtime_init(void) { return ESP_OK; }
esp_err_t wasm_runtime_load(const uint8_t *d, uint32_t l, uint8_t *id) { (void)d; (void)l; (void)id; return ESP_OK; }
esp_err_t wasm_runtime_start(uint8_t id) { (void)id; return ESP_OK; }
esp_err_t wasm_runtime_stop(uint8_t id) { (void)id; return ESP_OK; }
esp_err_t wasm_runtime_unload(uint8_t id) { (void)id; return ESP_OK; }
void wasm_runtime_on_timer(void) {}
void wasm_runtime_get_info(wasm_module_info_t *info, uint8_t *count) { (void)info; if(count) *count = 0; }
esp_err_t wasm_runtime_set_manifest(uint8_t id, const char *n, uint32_t c, uint32_t m) { (void)id; (void)n; (void)c; (void)m; return ESP_OK; }

/* ---- mmwave_sensor stubs (ADR-063) ---- */

#include "mmwave_sensor.h"

static mmwave_state_t s_stub_mmwave = {0};

esp_err_t mmwave_sensor_init(int tx, int rx) { (void)tx; (void)rx; return ESP_ERR_NOT_FOUND; }
bool mmwave_sensor_get_state(mmwave_state_t *s) { if (s) *s = s_stub_mmwave; return false; }
const char *mmwave_type_name(mmwave_type_t t) { (void)t; return "None"; }
