# ADR-081: I2S Audio Frame Format and Pin Layout

**Status:** Accepted (firmware shipped 0.8.0-audio)

**Date:** 2026-05-08

## Context

The fleet's XIAO ESP32-S3 nodes are gaining an I2S MEMS microphone (SPH0645LM4H-B compatible) for HomeView's audio sensing roadmap. We need:

1. A pin layout that doesn't conflict with the existing piezo speaker on D0/GPIO 1, and that we can re-use on every node going forward
2. A wire-format for shipping audio features over UDP, consistent with the existing `0xC511xxxx` frame magic family
3. A single firmware binary that supports any combination of speaker + mic per node — selected at boot via NVS, not by recompile

This ADR is the canonical record of those decisions. CLAUDE.md and `audio_mic.h` link here.

## Pin Layout (LOCKED)

| Sensor | XIAO silkscreen | ESP32-S3 GPIO | Role |
|---|---|---|---|
| Piezo PWM | D0 | **GPIO 1** | LEDC PWM output |
| Mic WS (LRCLK) | D1 | **GPIO 2** | I2S WS |
| *(skip D2 / GPIO 3 — strap pin)* | — | — | reserved |
| Mic SCK (BCLK) | D3 | **GPIO 4** | I2S BCLK |
| Mic SD (DIN) | D4 | **GPIO 5** | I2S data in |
| Mic VDD | 3V3 | — | 3.3V only — never 5V |
| Mic GND | GND | — | bridge L/R to GND for left-channel only |

**Why these pins:**
- D0/GPIO 1 was the piezo on the deployed fleet (.86 TV, .75 chair) before this ADR. Kept stable to avoid re-soldering.
- D2/GPIO 3 is an ESP32-S3 strapping pin that selects JTAG signal source at reset. The mic's SCK is a passive input that wouldn't disturb the strap, but skipping it keeps boot-time behavior simple and leaves GPIO 3 free if hardware JTAG ever becomes useful.
- GPIOs 2/4/5 are routable I2S targets via the ESP32-S3 GPIO matrix. No special function reserved.
- Display reservation on GPIOs 4-7 in the legacy firmware was for the optional AMOLED add-on board. The XIAOs in this fleet do not have a display, so the pins are functionally free here. ADR-045 / `display_hal.c` users on different hardware will need to choose mic pins differently — out of scope for this fleet.

**Important: `display_hal.c` claims GPIOs 4 and 5 even when no display is attached** because it unconditionally calls `spi_bus_initialize(SPI2_HOST, ...)` during init, which routes those pins to the SPI peripheral via the GPIO matrix. If `CONFIG_DISPLAY_ENABLE=y` and audio mic is also enabled, the I2S RX channel on GPIO 4/5 reads zeros after the first DMA buffer (the SPI peripheral steals the pins). The fleet `sdkconfig.defaults` sets `# CONFIG_DISPLAY_ENABLE is not set` to avoid this. Anyone re-enabling the display needs to either remove the mic from this board or move the mic to GPIOs that don't overlap the display QSPI bus.

## Single-Firmware NVS Gating

Same firmware binary runs on every fleet node. Each sensor gates on NVS at boot:

| Node profile | `piezo_gpio` | `mic_enable` |
|---|---|---|
| Speaker only (.86 TV, .75 chair) | 1 | 0 |
| Mic only (test board) | 255 | 1 |
| Speaker + mic | 1 | 1 |
| Bare CSI (.76, .77) | 255 | 0 |

NVS keys are remotely writable via the `/config/set` HTTP endpoint introduced in 0.7.1-config-http. New audio keys: `mic_enable`, `mic_ws`, `mic_sck`, `mic_sd`, `mic_sr`, `mic_shift`. All have validated ranges in `nvs_config.c` and `config_http.c`.

## Frame Format

New magic `0xC5110007`. 26-byte fixed header + variable feature payload. Initial implementation ships RMS + peak only; FFT bins are payload-reserved for a follow-up.

```
Offset  Size  Field            Notes
------  ----  -----            -----
0       4     Magic            0xC5110007 (LE)
4       1     Node ID
5       1     Format version   0x01
6       2     Sample rate Hz   u16 LE (e.g. 16000)
8       4     Window seq       u32 LE (monotonic per node, resets at boot)
12      4     Window start µs  u32 LE (esp_timer_get_time low 32 bits)
16      2     Window samples   u16 LE (e.g. 16000 for 1 s @ 16 kHz)
18      4     RMS              float32 LE (post-HPF, normalized -1..1)
22      4     Peak abs         float32 LE (post-HPF, normalized -1..1)
26      N     Optional FFT mag float32[N] (initially N=0; reserved)
```

The `0xC511` magic family at the time of this ADR:

| Magic | Meaning | Source |
|---|---|---|
| `0xC5110001` | CSI v1 | ADR-018 |
| `0xC5110002` | Vitals | edge_processing.h |
| `0xC5110003` | Feature vector | edge_processing.h, ADR-069 |
| `0xC5110004` | WASM events / fused vitals | wasm_runtime.h |
| `0xC5110005` | Compressed CSI | edge_processing.h |
| `0xC5110006` | CSI v2 with source MAC | csi_collector.h |
| `0xC5110007` | **Audio features (this ADR)** | audio_mic.h |

## SPH0645 Quirks the Firmware Compensates For

1. **DC bias** — first-order IIR high-pass filter applied before any feature math. Coefficient α = exp(-2π·f_c/f_s). Default cutoff 80 Hz (Kconfig `AUDIO_MIC_HPF_CUTOFF_HZ`). Without this, dc_post is nonzero and RMS is dominated by the bias.
2. **Sample alignment** — raw 32-bit I2S frames hold the 24-bit sample in the upper bits, with the bottom 8 bits as noise/zero. Default arithmetic right shift = **8 bits** (NVS-tunable 8-16 via `mic_shift`), which recovers the full 24-bit signed range that maps cleanly to -1.0..1.0 after dividing by 2^23 in `to_unit()`. The handoff doc suggested 14, which produced correct waveforms but compressed the headroom to 1/64 of full scale; shift=8 was empirically validated on node 4 (2026-05-08).
3. **Startup transient** — first 50 ms of samples discarded after I2S enable. Spec.

## Operational Policy

- **Off by default.** `mic_enable=0` is the NVS default. Existing nodes that OTA from 0.7.x to 0.8.0 keep their behavior unchanged — audio init only runs after explicit `/config/set mic_enable=1`.
- **Skipped in safe mode.** If audio init ever panics, the boot-loop counter trips and a clean recovery boot has audio off + OTA reachable. Same pattern as mmWave / WASM / display.
- **Server-side ingestion deferred.** The Rust sensing server's UDP listener silently drops unknown magics, so 0.8.0 firmware on a server that doesn't yet parse `0xC5110007` is safe — frames are noop'd. A follow-up PR adds the parser to `wifi-densepose-sensing-server`.

## Debug raw-PCM stream (firmware 0.8.1-audio-stream)

For end-to-end audio debugging — "is the mic actually picking up sound?" — the firmware exposes a bounded raw-PCM emission mode on top of the feature stream above. This is **not** a frame format with magic; it's just int16 LE bytes on the wire so anything (`sox`, `ffplay`, the bundled `wifi-densepose listen` CLI) can decode it without a parser.

### Endpoints

| Method | URI | Purpose |
|---|---|---|
| `POST` | `/audio/raw_stream/start?duration_s=N&port=P[&ip=A]` | Arm raw-PCM emission. `duration_s` ∈ [1, 300]. `port` ∈ [1024, 65535]. If `ip` is omitted the firmware streams to the HTTP peer (i.e. the caller). |
| `POST` | `/audio/raw_stream/stop` | Disarm before the deadline elapses. |
| `GET`  | `/audio/raw_stream/status` | `{active, remaining_ms, ip, port, chunks_sent, sample_rate, chunk_samples}` — also used by the CLI to discover the wire format. |

All three are gated by `ota_update_require_auth`. Auto-deadline + 300 s ceiling prevent a client crash from leaving the radio time hogged forever. Last-writer-wins on concurrent `/start` calls.

### Wire format

- Raw int16 LE mono, post-HPF (same DSP path as the feature stream — DC bias removed before saturating int32 → int16).
- 320-sample chunks per UDP datagram = **640 bytes payload**. At 32 kHz that's 10 ms cadence; at 16 kHz it's 20 ms.
- ~512 kbps at 32 kHz, ~256 kbps at 16 kHz. Single sample-rate UDP unicast.
- No header. Decode with `sox -t raw -r 32000 -e signed -b 16 -c 1` or equivalent.

### CLI integration

`wifi-densepose-cli` ships a `listen` subcommand that orchestrates the round-trip without `sox`/`nc`:

```
wifi-densepose listen --node 192.168.0.95 --duration_s 30 [--out capture.wav] [--no-play]
```

Binds a free local UDP port → posts `/start` (firmware reads peer IP automatically) → decodes int16 chunks → `cpal` for live playback and/or `hound` for WAV write. Cross-platform (macOS / Linux / Proxmox VM); no system audio deps beyond what `cpal` already brings in.

### OTA-while-mic-on

`audio_mic_pause`/`audio_mic_resume` were added and wired into `ota_quiesce_runtime` in this version. Without them, the audio task on core 1 starves the WiFi RX path enough that OTA POST hangs on any mic-enabled node. Pre-0.8.1 mic-enabled nodes can only be flashed via USB; from 0.8.1 onward, OTA is supported with the mic running.

## References

- `firmware/esp32-csi-node/main/audio_mic.{h,c}` — implementation
- `firmware/esp32-csi-node/main/stream_sender.{h,c}` — `stream_sender_send_to` for arbitrary IP+port debug sends
- `firmware/esp32-csi-node/main/nvs_config.c` — defaults and NVS overrides
- `firmware/esp32-csi-node/main/config_http.c` — `/config/set` whitelist
- `rust-port/wifi-densepose-rs/crates/wifi-densepose-cli/src/listen.rs` — listener CLI
- ADR-018 — original CSI frame format (this is the same `0xC511` family)
- ADR-080 — most recent prior ADR
- HomeView roadmap §"Tier 1: Sensing Pipeline" — audio integration target
