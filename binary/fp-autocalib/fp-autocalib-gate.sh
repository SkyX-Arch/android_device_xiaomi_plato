#!/vendor/bin/sh
#
# fp-autocalib-gate — Auto-detect display panel replacement and trigger
# Goodix fingerprint sensor recalibration when needed.
#
# Runs once per boot via init.fingerprint.rc. The script reads the
# current DRM panel identity from a sysfs node exposed by the Mediatek
# display driver. That identity changes whenever the actual screen is
# physically swapped (aftermarket repair). We compare it against the
# value persisted in `persist.vendor.sys.fp.last_panel_id` and, if it differs,
# wipe the stale Goodix calibration blobs and invoke the fp-autocalib
# helper which re-trains the sensor against the new panel.

set -eu

LOG_TAG="FP-AUTOCALIB"
CUR_PANEL=""

# Primary sysfs node — already chmod'ed by init.fingerprint.rc.
if [ -r /sys/class/drm/card0-DSI-1/disp_param ]; then
    CUR_PANEL=$(head -c 64 /sys/class/drm/card0-DSI-1/disp_param 2>/dev/null \
                | tr -d '\n' | tr ' ' '_')
fi

# Fallback to the Mediatek display sysfs location.
if [ -z "${CUR_PANEL}" ] && [ -r /sys/class/mi_display/disp-DSI-0/disp_param ]; then
    CUR_PANEL=$(head -c 64 /sys/class/mi_display/disp-DSI-0/disp_param 2>/dev/null \
                | tr -d '\n' | tr ' ' '_')
fi

# If no panel info is available (factory test units, unsupported panels),
# skip silently — we cannot make a decision.
if [ -z "${CUR_PANEL}" ]; then
    log -t "${LOG_TAG}" "no sysfs panel info available; skipping check"
    exit 0
fi

# Guard: if the helper binary failed to ship in the build, abort the
# early-return threshold BEFORE we wipe the calibration files. Wiping
# without the ability to recalibrate would leave the user worse off.
if [ ! -x /vendor/bin/fp-autocalib ]; then
    log -t "${LOG_TAG}" "fp-autocalib binary missing; skipping (would strand sensor)"
    exit 0
fi

SAVED_PANEL=$(getprop persist.vendor.sys.fp.last_panel_id || true)

# First boot after this script is deployed: just cache the current
# identity and exit. No history to compare against yet, and we do not
# want to nuke legitimate calibration on day one.
if [ -z "${SAVED_PANEL}" ]; then
    log -t "${LOG_TAG}" "first boot: caching panel_id=${CUR_PANEL}"
    setprop persist.vendor.sys.fp.last_panel_id "${CUR_PANEL}"
    exit 0
fi

# Same panel as last boot → sensor should already be correctly
# calibrated, do nothing.
if [ "${CUR_PANEL}" = "${SAVED_PANEL}" ]; then
    log -t "${LOG_TAG}" "panel_id stable (${CUR_PANEL}); no recalibration"
    exit 0
fi

# Panel was physically swapped → enter recalibration flow.
log -t "${LOG_TAG}" "panel swap detected: '${SAVED_PANEL}' -> '${CUR_PANEL}'"

# Wipe the stale factory calibration tables so libgf_hal.so will
# re-train from scratch on next enrolment.
rm -rf /data/vendor/goodix/* 2>/dev/null || true
rm -rf /mnt/vendor/persist/goodix/* 2>/dev/null || true
rm -rf /data/vendor/fpdump/* 2>/dev/null || true
sync

log -t "${LOG_TAG}" "wiped stale calibration data; calling fp-autocalib"

# Invoke factory-calibration helper. Exits 0 (EXIT_SUCCESS) on success.
if /vendor/bin/fp-autocalib; then
    log -t "${LOG_TAG}" "factoryCalibrate succeeded"
    setprop persist.vendor.sys.fp.last_panel_id "${CUR_PANEL}"
    setprop persist.vendor.sys.fp.last_calib_ts "$(date +%s)"
else
    rc=$?
    log -t "${LOG_TAG}" "factoryCalibrate failed (rc=${rc}); keeping old panel_id"
fi

exit 0
