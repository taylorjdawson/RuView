# Fork Cleanup Feature Readiness - 2026-05-27

## Baseline

- `main`, `origin/main`, and `upstream/main` are aligned at `04f205a0`.
- Safety branch: `taylor/safety/pre-cleanup-20260527` at pre-cleanup `per-node-classifier` tip `58cd860f`.
- Dirty and untracked pre-cleanup work is preserved in `stash@{0}`: `pre-cleanup-20260527 per-node-classifier dirty work`.

## Inventory

| Feature | Classification | Evidence | Subsystem | Training impact | Validation note |
| --- | --- | --- | --- | --- | --- |
| 62-dim per-node adaptive classifier | implemented-fork-only | `taylor/preserve/per-node-classifier-62` commit `5ba7488c`; original `feat/per-node-classifier` commit `2a23f247`; stash port `stash@{0}` | `v2/crates/wifi-densepose-sensing-server` adaptive classifier, CSI runtime, API server | Must preserve before training; changes adaptive model width from 15 to 62 when `node_features` are present | `cargo test -p wifi-densepose-sensing-server adaptive_classifier --no-default-features` passed; `cargo check -p wifi-densepose-sensing-server --no-default-features` passed with pre-existing Matter warnings |
| Source provenance / source-MAC framing | implemented-fork-only, blocked on protocol rebase | Original branch `feat/adr-018-source-mac` commits `29a7f8f4`, `45c7e952`; attempted cherry-pick onto `04f205a0` conflicted | ESP32 CSI wire format, hardware parser, sensing server parser, recording schema | Important before training if recordings must separate client/source links; otherwise per-node data may mix links | Blocked: conflicts in firmware, parser crates, sensing server parser, and `Cargo.lock`; branch magic `0xC5110006` now conflicts with upstream fused-vitals assignment, so assign a new magic before porting |
| Firmware reliability | implemented-fork-only, partially overlapping upstream | `taylor/firmware-hardening-1-5` commits `670f2437`, `a214d5d1`, `9c8f6ad3`; `custom`/`taylor/audio-raw-stream-cli` commit `1556868f` | ESP32 TWDT, boot health, remote NVS config, OTA, provisioning | Preserves data collection stability; bad firmware uptime creates bad training data | Not yet ported; carry forward generic reliability only, while keeping upstream fail-closed OTA PSK semantics |
| I2S MEMS / audio raw-stream | private-local-ops | `taylor/audio-raw-stream-cli` commit `1556868f`; earlier `cdc85950` | ESP32 I2S mic, UDP raw PCM debug stream, Rust CLI listener | Optional label-alignment infrastructure; should not block classifier training | Preserve privately due raw-audio privacy sensitivity; sanitize LAN/IP/fleet docs before any upstream proposal |
| Local deployment docs/scripts/node fleet knowledge | private-local-ops | `nodes.md`, `scripts/deploy-firmware.sh`, `firmware/esp32-csi-node/DEPLOYMENT.md` on `taylor/audio-raw-stream-cli` / `custom` | Local OTA workflow, fleet provisioning | Useful for repeatable local data collection | Keep private unless generalized and redacted |
| Matter / MQTT / Home Assistant publishing | implemented-upstream, runtime glue incomplete | `upstream/main` adds `v2/crates/wifi-densepose-sensing-server/src/mqtt/*`, `src/matter/*`, `docs/adr/ADR-115-home-assistant-integration.md` | HA/MQTT/Matter integration | Can provide label context, but no training ingestion path found | Treat as upstream baseline; add only minimal recording-label glue if needed |
| Semantic event labels | implemented-upstream, not wired to training | `upstream/main` adds `v2/crates/wifi-densepose-sensing-server/src/semantic/*` | Semantic primitives and HA entities | Useful only after label taxonomy is frozen | Do not use as classifier labels until runtime and recording/training semantics are explicit |
| NDP active probing | documented-upstream-not-implemented | Current firmware placeholder `csi_inject_ndp_frame()` and radio profile stubs | ESP32 active probing / radio ops | Potentially useful for capture protocol, but not required unless collection protocol depends on it | Defer unless active probing is explicitly required before recordings |

## Pre-Training Feature Set

1. Keep the 62-dim per-node classifier.
2. Port source provenance only after assigning a non-conflicting CSI magic and reconciling upstream parser changes.
3. Port only generic firmware reliability needed for stable collection.
4. Use HA/MQTT labels only if recording glue is low risk; do not depend on Matter or semantic runtime outputs for first training.

## Deferred

- Full ADR-103 learned multi-person counter.
- Audio-assisted training unless audio becomes explicit label supervision.
- UI polish unrelated to capture quality, label quality, or classifier input shape.
