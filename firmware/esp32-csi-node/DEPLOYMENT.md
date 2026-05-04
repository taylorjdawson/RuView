# ESP32 CSI Node — Deployment & Recovery Runbook

This runbook walks through deploying hardened firmware to the field nodes and
iterating on them remotely via OTA. The *first* flash for any node must
happen over USB so the safety net (boot crash counter, task watchdog, OTA
PSK) is in place before anyone trusts an OTA push.

---

## 0. Prerequisites

- ESP-IDF v5.4 toolchain (Docker recipe in this repo or a local install).
- Python 3.9+ with `esptool` and `esp-idf-nvs-partition-gen`:
  ```sh
  pip install esptool esp-idf-nvs-partition-gen pyserial
  ```
- Network access to the AP that the nodes will associate with.
- A list of the node IPs (e.g. `192.168.0.75`–`78` for the current fleet) and
  a USB-C cable for the initial flash of each.

---

## 1. Build the hardened firmware

```sh
cd firmware/esp32-csi-node
# 8 MB build (real WiFi CSI, no mocks):
cp sdkconfig.defaults.template sdkconfig.defaults
idf.py set-target esp32s3
idf.py build
```

Verify the build banner mentions:
- `boot_health` source file
- `esp_task_wdt` linkage (no warnings about missing symbols)
- `OTA bearer auth` if the PSK code path is exercised in logs

Note the version string in `build/esp32-csi-node.bin`'s app descriptor — you
should bump it before each release.

---

## 2. First-flash each node over USB

> Skipping this step and OTA-pushing straight to a vanilla node is the
> failure mode this hardening is designed to prevent.

For each physical device:

1. Plug the node in over USB-C, identify the port (`COM7`, `/dev/cu.usbmodem*`).
2. Erase any stale flash so the boot crash counter starts at 0:
   ```sh
   python -m esptool --chip esp32s3 --port <PORT> erase_flash
   ```
3. Flash the freshly built firmware:
   ```sh
   idf.py -p <PORT> flash
   ```
4. Provision NVS — **including the OTA PSK**:
   ```sh
   python provision.py --port <PORT> \
       --ssid "ssid" --password "secret" \
       --target-ip 192.168.0.20 --target-port 5005 \
       --node-id <N> \
       --ota-psk auto
   ```
   Save the printed PSK to your password manager. provision.py wipes the
   entire NVS partition on every run, so include `--ota-psk <stored value>`
   on every subsequent provision call or the device will fall back to
   permissive (no-auth) OTA.
5. Open the serial monitor and confirm the boot banner:
   ```
   I (xxx) main: ESP32-S3 CSI Node (ADR-018) — v0.X.Y — Node ID: N (boot crash_count=0)
   I (xxx) nvs_config: NVS override: ssid=...
   I (xxx) ota_update: OTA PSK loaded from NVS (64 chars) — authentication enabled
   I (xxx) main: Boot stability timer armed (30000 ms)
   I (xxx) main: CSI streaming active → 192.168.0.20:5005 (... mode=normal)
   ```
   If you see `mode=SAFE`, the device booted into recovery mode. See §6.

Repeat for every node in the fleet.

---

## 3. Verify each node is reachable for OTA

```sh
NODE=192.168.0.75
PSK=<the value from step 2.4>

# Health check — should return JSON with version + partition info
curl -sS -H "Authorization: Bearer $PSK" "http://$NODE:8032/ota/status"

# Without the PSK, the same endpoint should return 403:
curl -sS -o /dev/null -w "%{http_code}\n" "http://$NODE:8032/ota/status"
# expect: 403
```

If both checks pass for all nodes, the fleet is OTA-safe.

---

## 4. OTA flash workflow

For routine firmware updates after the fleet is hardened:

```sh
NODE=192.168.0.75
PSK=<value>
FW=build/esp32-csi-node.bin

curl -X POST -H "Authorization: Bearer $PSK" \
     --data-binary @"$FW" \
     "http://$NODE:8032/ota"
```

The OTA HTTP server quiesces CSI capture and the display task during the
upload (see `ota_quiesce_runtime` in `main.c`), then writes to the inactive
OTA partition and reboots into it. ESP-IDF's rollback support marks the
new image pending verify until the firmware reaches a steady state.

Watch the boot log on at least one node after each push to confirm:
- `boot crash_count=0` (clean reboot, not a panic)
- `mode=normal` (no safe-mode escalation)
- `Boot stability timer armed (30000 ms)`

After ~30 s of stable operation the firmware writes `crash_count=0` to NVS
and the new image is permanently accepted.

---

## 5. What "safe" actually buys you

| Failure | Without hardening | With hardening |
|---|---|---|
| Push firmware that panics in `app_main` | Brick — boot loops forever | Crash counter increments; after 5 consecutive panics the node skips WASM/swarm/mmWave/display and stays reachable on `:8032` |
| WiFi AP reboots | Node's STA client gives up after 10 retries, never reconnects | Backoff timer keeps retrying every 30 s indefinitely |
| Main task wedges (deadlock in user code) | Node looks alive but doesn't process packets | Task WDT fires after 30 s → panic → reboot → counter increments |
| Open OTA endpoint on a public/lab LAN | Anyone on the LAN can flash arbitrary firmware | 403 unless they have the bearer PSK |

---

## 6. Recovering a node that's in safe mode

1. Confirm via boot log: `mode=SAFE` and `crash_count=5` (or higher).
2. Build a known-good firmware locally.
3. OTA-flash it using the credentials from §4.
4. After reboot, watch for `mode=normal` and `crash_count=0` after the
   30 s stability window.

The recovery firmware does not need to be special — it just needs to *not
crash for 30 s*. If even your fallback firmware crashes, you've hit the
end of the OTA recovery path and the node needs a USB reflash.

---

## 7. Tuning knobs

All exposed via `idf.py menuconfig` → "Boot Health & Watchdog":

- `BOOT_STABILITY_DELAY_MS` (default 30 000) — how long the firmware must
  run before the crash counter is cleared. Raise if your steady-state
  initialization takes longer.
- `BOOT_SAFE_MODE_THRESHOLD` (default 5) — consecutive crashes before
  safe mode latches. Lower for stricter recovery, raise to tolerate more
  transient flaps.
- `MAIN_LOOP_WDT_TIMEOUT_S` (default 30) — task watchdog timeout for the
  application main loop. Lower for faster crash detection, raise if any
  long synchronous init exceeds it.
- `WIFI_RECONNECT_BACKOFF_MS` (default 30 000) — slow-retry interval after
  the initial 10 fast retries are exhausted.

---

## 8. Verification before declaring a deploy "done"

For every fleet-wide rollout:

- [ ] All nodes return version banner via `GET /ota/status` with PSK.
- [ ] All nodes reject `GET /ota/status` without PSK (HTTP 403).
- [ ] Aggregator logs show CSI frames from all node IDs at expected rate.
- [ ] Spot check one node's serial output for `mode=normal` after OTA.
- [ ] PSK is recorded in the team password manager, not in chat or commits.
- [ ] Run `make test` in `firmware/esp32-csi-node/test/` to confirm
      `boot_health` invariants haven't regressed in the source tree.
