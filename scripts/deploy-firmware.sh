#!/usr/bin/env bash
# Build + OTA-deploy the ESP32 CSI Node firmware.
#
# Designed to be run from a home server that sits on the same LAN as the
# fleet (see firmware/esp32-csi-node/DEPLOYMENT.md §8). Builds via the
# espressif/idf docker image so no native ESP-IDF install is required.
#
# Usage:
#   scripts/deploy-firmware.sh                              # build + push to default fleet
#   scripts/deploy-firmware.sh 192.168.0.86                 # build + push to one node
#   scripts/deploy-firmware.sh --skip-build 192.168.0.86    # push existing build, no rebuild
#   scripts/deploy-firmware.sh --verify-only                # just hit /ota/status on each node
#
# Exit codes:
#   0  all targeted nodes responded with the new version after OTA
#   1  build failed
#   2  one or more nodes failed to OTA or didn't return after reboot

set -uo pipefail

# Default fleet — keep in sync with nodes.md.
DEFAULT_FLEET=(
    192.168.0.75
    192.168.0.76
    192.168.0.77
    192.168.0.78
    192.168.0.86
)

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
FW_DIR="$REPO_ROOT/firmware/esp32-csi-node"
FW_BIN="$FW_DIR/build/esp32-csi-node.bin"
IDF_IMAGE="espressif/idf:release-v5.4"
OTA_PORT=8032
REBOOT_WAIT_S=10

SKIP_BUILD=0
VERIFY_ONLY=0
TARGETS=()

usage() {
    sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-build) SKIP_BUILD=1; shift ;;
        --verify-only) VERIFY_ONLY=1; SKIP_BUILD=1; shift ;;
        -h|--help) usage ;;
        --) shift; TARGETS+=("$@"); break ;;
        -*) echo "unknown flag: $1" >&2; exit 2 ;;
        *) TARGETS+=("$1"); shift ;;
    esac
done

if [[ ${#TARGETS[@]} -eq 0 ]]; then
    TARGETS=("${DEFAULT_FLEET[@]}")
fi

# --- 1. Build (skipped for --skip-build / --verify-only) -------------------

if [[ $SKIP_BUILD -eq 0 ]]; then
    echo "=== Building firmware via $IDF_IMAGE ==="
    if ! command -v docker >/dev/null 2>&1; then
        echo "docker not installed — see DEPLOYMENT.md §8.2" >&2
        exit 1
    fi
    docker run --rm \
        -v "$FW_DIR:/project" \
        -w /project \
        "$IDF_IMAGE" \
        idf.py build || {
            echo "build failed" >&2
            exit 1
        }
fi

if [[ $VERIFY_ONLY -eq 0 ]]; then
    if [[ ! -f "$FW_BIN" ]]; then
        echo "no firmware binary at $FW_BIN — run without --skip-build first" >&2
        exit 1
    fi
    FW_SIZE=$(stat -f %z "$FW_BIN" 2>/dev/null || stat -c %s "$FW_BIN")
    echo "firmware: $FW_BIN ($FW_SIZE bytes)"
fi

# --- 2. Pre-deploy snapshot -------------------------------------------------

echo
echo "=== Pre-deploy fleet status ==="
for ip in "${TARGETS[@]}"; do
    resp=$(curl -sS --max-time 3 "http://$ip:$OTA_PORT/ota/status" 2>/dev/null || true)
    if [[ -n "$resp" ]]; then
        echo "  $ip: $resp"
    else
        echo "  $ip: unreachable"
    fi
done

if [[ $VERIFY_ONLY -eq 1 ]]; then
    exit 0
fi

# --- 3. OTA push (sequential) ----------------------------------------------

echo
echo "=== Pushing firmware ==="
PUSH_FAILED=()
for ip in "${TARGETS[@]}"; do
    echo -n "  $ip: "
    if curl -sS --max-time 60 -X POST \
        --data-binary "@$FW_BIN" \
        -H "Content-Type: application/octet-stream" \
        "http://$ip:$OTA_PORT/ota" 2>/dev/null | grep -q '"status":"ok"'; then
        echo "ok"
    else
        echo "FAILED"
        PUSH_FAILED+=("$ip")
    fi
done

# --- 4. Wait, then verify each came back ------------------------------------

echo
echo "=== Waiting ${REBOOT_WAIT_S}s for reboots ==="
sleep "$REBOOT_WAIT_S"

echo
echo "=== Post-deploy fleet status ==="
VERIFY_FAILED=()
for ip in "${TARGETS[@]}"; do
    resp=$(curl -sS --max-time 5 "http://$ip:$OTA_PORT/ota/status" 2>/dev/null || true)
    if [[ -n "$resp" ]]; then
        echo "  $ip: $resp"
    else
        echo "  $ip: did not return after reboot"
        VERIFY_FAILED+=("$ip")
    fi
done

# --- 5. Summary -------------------------------------------------------------

echo
if [[ ${#PUSH_FAILED[@]} -eq 0 && ${#VERIFY_FAILED[@]} -eq 0 ]]; then
    echo "=== ALL ${#TARGETS[@]} NODES OK ==="
    exit 0
fi

echo "=== DEPLOY HAD FAILURES ==="
[[ ${#PUSH_FAILED[@]} -gt 0 ]] && echo "  push failed: ${PUSH_FAILED[*]}"
[[ ${#VERIFY_FAILED[@]} -gt 0 ]] && echo "  no response after reboot: ${VERIFY_FAILED[*]}"
echo
echo "Recover bricked nodes via USB — see DEPLOYMENT.md §6."
exit 2
