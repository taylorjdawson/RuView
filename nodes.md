# HomeView Deployment Tracker

## Node Registry

| Node ID | MAC Address       | TDM Slot | Location              | IP Address   | Hardware       | Firmware          |
|---------|-------------------|----------|-----------------------|--------------|----------------|-------------------|
| 0       | 1c:db:d4:75:43:18 | 0 of 5   | TV (USB-powered)      | 192.168.0.86 | piezo only     | 0.8.0-audio       |
| 1       | ac:a7:04:2c:4b:64 | 1 of 5   | Bedroom               | 192.168.0.75 | piezo only     | 0.8.0-audio       |
| 2       | ac:a7:04:2c:4b:c8 | 2 of 5   |                       | 192.168.0.76 | bare CSI       | 0.8.0-audio       |
| 3       | 1c:db:d4:75:43:b8 | 3 of 5   |                       | 192.168.0.77 | bare CSI       | 0.8.0-audio       |
| 4       | 1c:db:d4:74:5f:bc | 4 of 5   | Behind easy chair     | 192.168.0.95 | I2S mic only   | 0.8.0-audio       |

Spare / decommissioned: `ac:a7:04:2c:54:ac` (was node 0 on stock, lease was .78).

**Hardware:** 5x XIAO ESP32-S3 (8MB flash, 8MB PSRAM) + external U.FL antennas
**WiFi:** nokings (ch 11)
**Aggregator:** 192.168.0.56:5005 (UDP)
**Firmware:** all 5 nodes on 0.8.0-audio (PR 1.5 hardening + remote NVS config
+ ADR-081 I2S mic). Audio is NVS-gated; only node 4 has `mic_enable=1`. Piezo
NVS-gated; only nodes 0/1 have `piezo_gpio=1`. Future updates OTA via
`./scripts/deploy-firmware.sh`.

**TDM note:** TDM was originally provisioned as 4 of 4. With node 4 added,
slot count should be bumped to 5. Update via:
`curl -X POST "http://<ip>:8032/config/set?key=tdm_slot&value=N"` once tdm_slot/tdm_total are added to the /config/set whitelist (currently piezo_* and mic_* only — straightforward extension when needed).

> **Stock → hardened upgrade requires USB.** Stock firmware was built with
> a partition layout where each OTA slot is 900 KB. The hardened firmware
> is 1.1 MB. OTA only rewrites the app slot — it cannot resize partitions
> — so the stock-to-hardened jump must be USB. Once a node is on
> hardened firmware (2 MiB slots), every subsequent push is OTA.

## Sensing Server
- **Port:** 3847
- **UI:** http://localhost:3847/ui/index.html
- **API:** http://localhost:3847/api/v1/
- **Start:** `cd rust-port/wifi-densepose-rs && cargo run -p wifi-densepose-sensing-server -- --http-port 3847 --source esp32`

## Quick Reference
```bash
# Record a session
curl -X POST http://localhost:3847/api/v1/recording/start -H "Content-Type: application/json" -d '{"session_name": "NAME", "label": "LABEL"}'
curl -X POST http://localhost:3847/api/v1/recording/stop

# Retrain classifier (files must be named train_*.jsonl with label keywords in filename)
curl -X POST http://localhost:3847/api/v1/adaptive/train

# Flash NVS to a node (USB)
python3 provision.py --port /dev/cu.usbmodem* --ssid "nokings" --password '...' --target-ip 192.168.0.56 --node-id N --target-port 5005 --tdm-slot N --tdm-total 4 --dry-run
esptool --chip esp32s3 --port /dev/cu.usbmodem* --baud 460800 write-flash 0x9000 nvs_provision.bin

# Check all nodes online
arp -a | grep -i "espressif\|a7:4:2c\|db:d4:75"
```

---

## Phase 1: Deploy RuView Stock — COMPLETE (2026-03-20)

- [x] Install prerequisites (Rust 1.88, Docker 28.3, esptool 5.2)
- [x] Clone RuView
- [x] Configure sdkconfig for XIAO ESP32-S3 (disable display/WASM, enable PSRAM)
- [x] Build firmware with ESP-IDF v5.2 Docker (797KB binary, 62% partition free)
- [x] Flash all 4 nodes via USB
- [x] Provision WiFi credentials + aggregator IP + node IDs (0-3)
- [x] Provision TDM slots (4-node mesh)
- [x] Attach external U.FL antennas (RSSI improved from -87 to -60 dBm)
- [x] Verify all 4 nodes streaming CSI to aggregator
- [x] Run sensing server with web UI
- [x] Initial calibration recordings (7 sessions: 1p still/moving/active + 2p still/moving + absent)
- [x] Add 2-person classes to adaptive classifier (7 classes total)
- [x] Train adaptive classifier (83K frames, 2P_STILL at 75% confidence working)

### What works now
- All 4 nodes on WiFi, streaming CSI over UDP
- Adaptive classifier: 7 classes (absent, present_still, present_moving, active, 2p_still, 2p_moving, 2p_active)
- 2-person detection working (2P_STILL correctly identified at 75% confidence)
- Web UI with live CSI visualization, signal features, classification

### What doesn't work yet
- Spatial positioning (no per-node breakdown, everything merged into one stream)
- Absent detection (0% — recording too short)
- 1-person motion classes (5-12% — need more data)
- Server shows "1 ESP32" despite 4 nodes connected
- RSSI values incorrect in UI (showing positive values)
- Pose skeleton is signal-derived guesswork, not trained

---

## Phase 2: Burn-In & Calibration — IN PROGRESS

**Duration:** 1-2 weeks, mostly passive. Run the nodes 24/7 and monitor.

### Passive Monitoring
- [ ] Node uptime — do any crash/reboot? Check with `arp -a` periodically
- [ ] CSI frame rate — steady ~50Hz per node or drops?
- [ ] WiFi reconnection — how do nodes handle router reboots?
- [ ] Memory — does ESP32 heap shrink over time (leak)?
- [ ] Server stability — does sensing server handle 4 nodes sustained?

### Infrastructure
- [ ] Fill in node physical locations in registry table
- [ ] Set static DHCP leases on router for all 4 MACs
- [ ] Move sensing server to always-on machine (Proxmox/NUC) — laptop sleep kills it

### Recordings Needed (physical, ~30 min total)
- [ ] **Empty apartment** — 2+ min, both people leave (fixes 0% absent accuracy)
- [ ] **1 person walking** — 2 min, varied paths through all rooms (fixes 5% present_moving)
- [ ] **1 person active** — 2 min, cleaning/cooking/exercising (fixes 12% active)
- [ ] **2 people active** — 2 min, both doing vigorous movement (currently 0 data)
- [ ] **2 people same room still** — 60s, both sitting on couch
- [ ] **2 people one moving one still** — 60s, one walks while other sits
- [ ] **Different times of day** — repeat key recordings morning/night
- [ ] **Doors open vs closed** — multipath changes significantly
- [ ] Retrain adaptive classifier after new recordings — target >80% all classes

### Current Classifier Accuracy (2026-03-21, balanced retrain)
| Class | Accuracy | Samples | Notes |
|-------|----------|---------|-------|
| absent | 91% | 91,045 | Fixed with 5min empty + downsampled |
| present_still | 88% | 38,739 | Stable — 4 locations |
| present_moving | 5% | 42,804 | Still weak — needs per-node features (3A.1) |
| active | 0% | 9,510 | Needs more varied data |
| 2p_still | 67% | 44,203 | Improved with living room recording |
| 2p_moving | 8% | 10,206 | Needs more data |
| 2p_active | 0% | 1 | Not recorded |

**Note:** All recordings above use merged features (pre-3A). Must re-record with per-node server for 3A.1.

---

## Phase 3: Fork & Multi-Node Intelligence — NOT STARTED

**Goal:** Transform from single-stream sensing to true multi-node spatial awareness. This is what makes 4 nodes actually better than 1.

### 3A: Per-Node CSI Separation (server software) — COMPLETE (2026-03-21)
**PR:** https://github.com/taylorjdawson/RuView/pull/1

- [x] Modify sensing server UDP handler to track frames per node_id (NodeState HashMap)
- [x] Store per-node CSI features independently (amplitude, phase, variance per node)
- [x] Add API endpoint: `GET /api/v1/nodes` — per-node health, frame rate, RSSI
- [x] Add per-node signal features to WebSocket broadcast (PerNodeFeatureInfo)
- [x] Update UI sensing tab to show 4 separate node signal panels
- [x] Fix "1 ESP32" display to show actual node count
- [x] Fix RSSI display (saturating_neg for sign correction)
- [x] Fused features (compute_fused_features) for backward compatibility
- [x] Node timeout: stale after 5s, removed after 30s
- [x] XSS fix: DOM element creation instead of innerHTML
- [x] Signal field uses fused features instead of single-node

**Result:** Each node's view is independent. UI shows per-node cards with RSSI, variance, classification.

### 3A.1: Per-Node Classifier Upgrade (server software) — NEXT
**Priority: HIGHEST — leverages 3A for dramatically better classification**
**Depends on:** 3A (complete)

The adaptive classifier currently uses 15 merged features. With per-node separation, it should use per-node features for spatial awareness.

- [ ] Update recording format: include `node_features` array in JSONL frames (per-node features, RSSI, classification)
- [ ] Update `features_from_frame` in adaptive_classifier.rs to extract per-node features from recordings
- [ ] Expand feature vector: 15 features × N nodes + cross-node features (variance spread, max node, RSSI gradient)
- [ ] Update classifier to handle variable node count (pad to max 8 nodes, zero-fill absent nodes)
- [ ] **Re-record all calibration sessions** with new server (old recordings lack per-node data)
  - Empty apartment (2+ min)
  - 1p still (office, couch, bed, bathroom)
  - 1p walking (4 min, all rooms)
  - 1p active (2 min)
  - 2p still (same room, different rooms)
  - 2p moving (living room)
  - 2p active (2 min)
- [ ] Retrain classifier on per-node features — target >80% all classes
- [ ] Add room-aware classes (optional): instead of just "present_still", detect "still_in_bedroom" vs "still_in_office"
- [ ] BLE beacon triangulation for automatic position labeling during recording walks (carry phone, nodes scan BLE RSSI → ground truth position)

**Result:** Classifier uses spatial signal from 4 nodes. "Node 0 high variance, others low" = person near node 0. Should dramatically improve accuracy for all motion classes and enable room-level awareness without pose estimation.

### 3B: Multi-Node Attention Fusion (server software)
**Priority: HIGH — this is the spatial intelligence layer**

Combines per-node CSI using attention weights based on geometric diversity (ADR-029 design).

- [ ] Implement cross-node correlation matrix (which nodes agree on motion?)
- [ ] Attention-weighted fusion: nodes closer to activity get higher weight
- [ ] Per-link CSI analysis: 4 nodes = 6 unique links (C(4,2)), each link detects motion along its path
- [ ] Room fingerprinting: learn which per-node signal pattern = which room
- [ ] Room-level localization: "person in bedroom" based on per-link signal strength
- [ ] Multi-person separation: different nodes see different people
- [ ] Add floor plan view to UI with per-room occupancy indicators
- [ ] Recording sessions for room fingerprints (60s standing in each room)

**Result:** "Person A in bedroom, Person B in office" — actual spatial awareness.

### 3C: NDP Active Probing (firmware)
**Priority: MEDIUM — improves CSI quality and consistency**

Currently nodes passively capture CSI from ambient WiFi traffic (unpredictable, variable quality). Active probing sends known frames between nodes for consistent measurements.

- [ ] Activate `csi_inject_ndp_frame()` stub in firmware (already exists in csi_collector.c)
- [ ] Implement proper 802.11 Null Data Packet frame construction
- [ ] Coordinate NDP schedule with TDM slots (node 0 sends, others measure, then node 1, etc.)
- [ ] Each TDM slot: transmit NDP → all other nodes capture CSI from it → 3 measurements per slot
- [ ] Server receives labeled CSI: "this frame is from node 0's NDP, captured by node 2"
- [ ] Rebuild firmware with Docker, OTA flash to all 4 nodes

**Result:** Consistent, high-quality CSI at known intervals instead of random ambient captures. 12 directed measurements per TDM cycle (4 transmitters × 3 receivers each).

### 3D: Clock Synchronization (firmware)
**Priority: MEDIUM — needed for phase coherence across nodes**

ESP32 crystal oscillators drift independently (~20-40 ppm). For combining phase measurements across nodes, they need shared time reference.

- [ ] Implement ESP-NOW beacon sync: one node (node 0) broadcasts timing beacon every 100ms
- [ ] Other nodes align their CSI timestamps to beacon
- [ ] Target: <1ms sync accuracy (sufficient for 20Hz CSI sampling)
- [ ] Add sync status to node health reports
- [ ] Alternative: use WiFi FTM (802.11mc) for ranging + time sync in one step

**Result:** CSI phase measurements from all 4 nodes can be coherently combined for breathing detection and fine motion.

### 3E: Reliability & OTA (firmware)
**Priority: LOW initially — do after 3A-3D prove the concept**

- [ ] Fork RuView repo to your GitHub
- [ ] R1: Supervisor + Watchdog (TWDT, WiFi reconnect, heap monitoring)
- [ ] R2: OTA with rollback (HTTP endpoint already exists, add validation)
- [ ] R3: Boot loop detection (NVS counter, safe mode after 5 crashes)
- [ ] R4: Coredump to flash (remote crash debugging)

### 3F: Future Hardware Extensions (requires soldering)
- [ ] H1: MQTT Client (remote config, health reporting)
- [ ] H2: BLE Scanning (device tracking, room presence via phone BLE)
- [ ] H3: Audio / I2S with INMP441 mic (sound event detection)
- [ ] H4: Piezo chirp / operator speaker path (ultrasonic ranging, ~1-5cm precision if needed)
- [ ] H4a: Configurable piezo defaults via Kconfig + NVS (`gpio`, `freq`, `duration`, `gap`, `duty`)
- [ ] H4b: Command surface for piezo playback (USB serial first for bench tests, OTA HTTP on WiFi)
- [ ] H4c: Validation pass: offline USB speaker smoke test, then OTA-triggered tone test on WiFi

---

## Phase 4: Training & ML — NOT STARTED

**Depends on:** Phase 3A-3B (per-node data + room fingerprints)

### 4A: Improve Adaptive Classifier
- [ ] Complete Phase 2 recordings
- [ ] Retrain with per-node features (from 3A) — much richer feature set
- [ ] Target >90% accuracy on all classes
- [ ] Add room-aware classes (bedroom_still, office_still, etc.)

### 4B: Pretrain Contrastive Model
- [ ] Run contrastive pretraining on all accumulated CSI recordings
- [ ] Learns apartment RF environment (multipath, wall effects, furniture)
- [ ] Foundation for supervised training and LoRA

### 4C: Supervised Training (physical presence required)
- [ ] Structured walks: enter room → walk to center → sit → stand → leave
- [ ] Label each segment with position and activity
- [ ] Train pose estimation model on labeled data
- [ ] Evaluate: does per-node CSI improve joint estimation vs single-node?

### 4D: LoRA Per-Room Profiles
- [ ] Create LoRA adapter per room
- [ ] Train on room-specific CSI patterns
- [ ] Auto-switch profiles based on room fingerprint from 3B

### 4E: Person Identification & Vitals
- [ ] Gait recognition: each person walks naturally, model learns to distinguish who
- [ ] Vital signs calibration: stationary subject, known position, 5 min
- [ ] Breathing rate and heart rate extraction from CSI phase
- [ ] WiFi FTM ranging for position refinement

---

## Architecture Notes

### What makes 4 nodes better than 1
- 1 node = 1 viewpoint, presence/motion only
- 4 nodes = 6 sensing links (C(4,2)), each link detects motion along its path
- With per-node separation (3A): can tell which links are disturbed → which room
- With attention fusion (3B): weighted combination → precise localization
- With active probing (3C): 12 directed measurements per cycle → consistent data
- With clock sync (3D): coherent phase → breathing/vital signs from across room

### Adaptive classifier classes
```
0: absent          — nobody home
1: present_still   — 1 person sitting/lying
2: present_moving  — 1 person walking
3: active          — 1 person vigorous movement
4: 2p_still        — 2 people sitting/lying
5: 2p_moving       — 2 people walking
6: 2p_active       — 2 people vigorous movement
```
Filename keywords for training: files must start with `train_` and contain the class keyword (e.g., `train_2p_still_couch.jsonl`).

---

## Future: Per-Device CSI Sensing (Design Doc)

**Status:** Design only — not started. Implement after Phase 3A.1 and room fingerprinting are solid.

### The Idea

Every WiFi device in the apartment (phones, laptops, smart devices) generates traffic that creates CSI signatures at all 4 nodes. Currently we capture this but don't know which device caused which CSI frame. Adding source MAC tracking turns 4 sensing nodes into a device-aware spatial mesh.

### Approach: Hybrid Per-Link Tracking (Option 3)

Keep all 4 nodes in promiscuous mode. Extend tracking to group by `(node_id, source_mac)`:

```
Current:  NodeState keyed by node_id → 4 sensing streams
Proposed: PerLinkState keyed by (node_id, source_mac) → N×M sensing links
          (4 nodes × 5 devices = 20 links)
```

Each link tells you: "how does the RF path between device X and node Y look right now?"

### What this enables

- **Device-specific tracking:** "Taylor's phone is near node 2" (because the phone→node2 CSI link shows strongest direct path)
- **Person identification without ML:** Each person carries their phone. Phone MAC → person identity. No gait recognition needed.
- **Passive room localization:** Phone near node 0 = living room, phone near node 2 = bedroom. Works even when people are stationary.
- **Object tracking:** Laptop on desk creates a static CSI baseline. When laptop moves, that link changes.
- **20 sensing links vs 4:** Massively more spatial information for the classifier.

### Implementation (2 changes)

**Firmware change (small):**
The CSI callback in `csi_collector.c` already receives `info->mac` (source MAC of the frame). Currently not included in UDP packet. Add 6 bytes of source MAC to the ADR-018 frame format after the existing header:

```c
// In csi_serialize_frame(), after existing header (offset 20):
// Add source MAC at offset 20, shift I/Q data to offset 26
memcpy(&buf[20], info->mac, 6);
// I/Q data now starts at offset 26
```

Update `Esp32CsiParser::parse_frame` in the Rust hardware crate to extract the MAC.

**Server change (medium):**
Extend per-node tracking to per-link:

```rust
struct PerLinkState {
    node_id: u8,
    source_mac: [u8; 6],
    frame_history: VecDeque<Vec<f64>>,
    latest_features: Option<FeatureInfo>,
    last_seen: Instant,
    frame_count: u64,
}

// In AppStateInner:
link_states: HashMap<(u8, [u8; 6]), PerLinkState>,
```

Add API endpoint: `GET /api/v1/links` — returns per-link features grouped by source device.

### Classifier impact

Feature vector expands from `N_nodes × 7` to `N_links × 7`. With 4 nodes and 5 known devices, that's 20 × 7 = 140 per-link features + cross-link features. The classifier would learn device-specific room signatures:

- "Phone A strong on node 0, weak on node 2" → person A in living room
- "Phone B strong on node 2, weak on node 0" → person B in bedroom
- Both = two people, specific rooms identified

### Prerequisites
- Phase 3A (per-node separation) — done
- Phase 3A.1 (per-node classifier) — done
- Device MAC discovery (need to know which MACs to track — could auto-discover from CSI traffic)

### Effort estimate
- Firmware: ~2 hours (add 6 bytes to frame format)
- Server: ~4 hours (per-link tracking, API endpoint, UI)
- Classifier: ~2 hours (extend feature vector for per-link)
- Rebuild + OTA flash: ~1 hour

---

### Key file locations
- Firmware: `firmware/esp32-csi-node/`
- Sensing server: `rust-port/wifi-densepose-rs/crates/wifi-densepose-sensing-server/`
- Adaptive classifier: `.../src/adaptive_classifier.rs` (modified: 7 classes)
- Recordings: `rust-port/wifi-densepose-rs/data/recordings/`
- Models: `rust-port/wifi-densepose-rs/data/models/`
- UI: `ui/`
- sdkconfig: `firmware/esp32-csi-node/sdkconfig.defaults` (modified: XIAO S3 config)
