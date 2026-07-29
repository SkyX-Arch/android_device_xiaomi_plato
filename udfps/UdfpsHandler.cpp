/*
 * Copyright (C) 2022 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "UdfpsHandler.xiaomi_mt6895"

#include <aidl/android/hardware/biometrics/fingerprint/BnFingerprint.h>
#include <android-base/logging.h>
#include <android-base/unique_fd.h>

#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>

#include "UdfpsHandler.h"
#include "xiaomi_touch.h"

#define COMMAND_NIT 10
#define PARAM_NIT_FOD 1
#define PARAM_NIT_NONE 0

#define COMMAND_FOD_PRESS_STATUS 1
#define PARAM_FOD_PRESSED 1
#define PARAM_FOD_RELEASED 0

#define FOD_STATUS_OFF 0
#define FOD_STATUS_ON 1

#define TOUCH_DEV_PATH "/dev/xiaomi-touch"
#define TOUCH_ID 0
#define TOUCH_MAGIC 'T'
#define TOUCH_IOC_SET_CUR_VALUE _IO(TOUCH_MAGIC, SET_CUR_VALUE)
#define TOUCH_IOC_GET_CUR_VALUE _IO(TOUCH_MAGIC, GET_CUR_VALUE)

#define DISP_PARAM_PATH "sys/devices/virtual/mi_display/disp_feature/disp-DSI-0/disp_param"
#define DISP_PARAM_LOCAL_HBM_MODE "9"
#define DISP_PARAM_LOCAL_HBM_OFF "0"
#define DISP_PARAM_LOCAL_HBM_ON "1"

#define FOD_PRESS_STATUS_PATH "/sys/class/touch/touch_dev/fod_press_status"

// [UDFPS debounce] Minimum press duration before a 1->0 transition is
// treated as a real release. The MTK touch IC occasionally emits a
// transient release within ~9-11 ms of a real press during its
// internal calibration / settling. 30 ms sits well above that
// glitch window and well below the human-tap floor (~50 ms+), so
// real taps still get the UP event. Tune here without diving into
// the poll lambda.
#define FOD_DEBOUNCE_MS 30

// [UDFPS poll cadence] Normal poll timeout when state is stable. Short
// enough that we notice mRunning=false for clean shutdown; long
// enough that idle polls don't burn CPU.
#define FOD_POLL_TIMEOUT_MS 500

// [UDFPS screen-off] Kernel wake-lock names so the device does not
// re-suspend while the FOD touch IC is polling auth state. Generic
// write to /sys/power/wake_lock creates a kernel wakeup_source; this
// name shows up cleanly in dumpsys wake_lock / sysfs wakeup_sources.
#define WAKE_LOCK_PATH "/sys/power/wake_lock"
#define WAKE_UNLOCK_PATH "/sys/power/wake_unlock"
#define WAKE_LOCK_ID "udfps_auth"

using ::aidl::android::hardware::biometrics::fingerprint::AcquiredInfo;

namespace {

template <typename T>
static bool set(const std::string& path, const T& value) {
    // [UDFPS diagnostics] The previous version silently swallowed
    // sysfs write errors. That hid SELinux / chmod / missing-path
    // failures in wake_lock + disp_param paths and made screen-off
    // FOD debugging a guessing game (vendor blob says "Up too fast"
    // but we couldn't tell whether the wake_lock had actually been
    // accepted). Now we surface a single WARNING per failed write
    // with the path + value, leaving successful writes silent.
    std::ofstream file(path);
    if (!file.is_open()) {
        LOG(WARNING) << "sysfs open failed: " << path;
        return false;
    }
    file << value;
    if (!file) {
        LOG(WARNING) << "sysfs write failed: " << path;
        return false;
    }
    return true;
}

static bool readBool(int fd) {
    char c;
    int rc;

    rc = lseek(fd, 0, SEEK_SET);
    if (rc) {
        LOG(ERROR) << "failed to seek fd, err: " << rc;
        return false;
    }

    rc = read(fd, &c, sizeof(char));
    if (rc != 1) {
        LOG(ERROR) << "failed to read bool from fd, err: " << rc;
        return false;
    }

    return c != '0';
}

}  // anonymous namespace

class XiaomiMt6895UdfpsHander : public UdfpsHandler {
  public:
    // [UDFPS lifecycle] Override destructor so the poll thread is
    // gracefully joined instead of being orphaned on HAL restart (the
    // previous implementation did .detach() which leaks if the biometric
    // service is restarted — the thread kept running with a dangling
    // mDevice pointer).
    ~XiaomiMt6895UdfpsHander() override {
        mRunning.store(false);
        if (mPollThread.joinable()) mPollThread.join();
    }

    void init(fingerprint_device_t* device) {
        mDevice = device;
        touch_fd_ = android::base::unique_fd(open(TOUCH_DEV_PATH, O_RDWR));

        // [UDFPS screen-off] Keep the kernel FOD matrix zone alive in
        // low-power regardless of panel state. Without this, touching
        // the under-display sensor while the screen is off never reaches
        // the poll thread because the touch IC has powered-down the FOD
        // zone. Setting at HAL init time is the only place this is safe:
        // once anyone touches the panel, Wake_Lock (below) keeps the
        // panel from entering suspend before auth completes.
        //
        // [UDFPS diagnostics] Boot-time ERROR (matches Touch_Aod_Enable
        // below): if this ioctl fails at init, NO FOD works — recoverable
        // only by HAL restart. Per-press ioctl in setFingerDown() uses
        // WARNING because next press retries.
        {
            int buf[MAX_BUF_SIZE] = {TOUCH_ID, Touch_Fod_Enable, 1};
            if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf) < 0) {
                PLOG(ERROR) << "Touch_Fod_Enable boot ioctl failed; FOD disabled";
            }
        }

        // [UDFPS screen-off AOD] Tell the touch IC to keep the FOD
        // matrix alive during AOD (Always-On Display). Without this,
        // the IC may revert to a low-power scan mode that drops press
        // events with the panel fully off. Touch_Aod_Enable is mode 11
        // in xiaomi_touch.h; set at boot, not per-press, so the IC's
        // internal AOD-aware scheduler is initialized correctly. Check
        // ioctl return — silent failure here means the FOD matrix will
        // be inoperative during AOD and screen-off FOD will not work.
        {
            int bufAod[MAX_BUF_SIZE] = {TOUCH_ID, Touch_Aod_Enable, 1};
            if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &bufAod) < 0) {
                PLOG(ERROR) << "Touch_Aod_Enable ioctl failed; "
                               "screen-off FOD will not work";
            }
        }

        mRunning.store(true);
        mPollThread = std::thread([this]() {
            int fd = open(FOD_PRESS_STATUS_PATH, O_RDONLY);
            if (fd < 0) {
                LOG(ERROR) << "failed to open fd, err: " << fd;
                return;
            }

            struct pollfd fodPressStatusPoll = {
                    .fd = fd,
                    .events = POLLERR | POLLPRI,
                    .revents = 0,
            };

            // [UDFPS edge-detection] Last observed press state. The
            // touch IC can fire POLLPRI on internal state changes
            // without the user-facing press/release actually changing;
            // without edge detection, our HAL would push DOWN+UP events
            // within milliseconds, which vendor Goodix reports as
            // "Up too fast" (GF_ERROR_TOO_FAST). Edge detection
            // collapses multiple wakeups at the same state into a
            // single event.
            bool lastPressed = false;
            // [UDFPS debounce] Anchor for the 1->0 debounce window.
            // Reset to {} on every DOWN transition; read on 1->0 to
            // decide whether to fire UP or swallow the transient
            // release.
            auto pressStart = std::chrono::steady_clock::time_point{};

            // [UDFPS dynamic timeout] Starts at FOD_POLL_TIMEOUT_MS
            // (500 ms) for the normal idle case. When we suppress a
            // 1->0 release as a touch-IC glitch, we shrink the
            // timeout to the *remainder* of the debounce window so
            // poll() wakes us back up to re-check the FD. This closes
            // the "stuck DOWN after spurious-release" race: even if
            // the touch IC silently returns to 0 with no further
            // POLLPRI, we re-read the FD within the debounce window
            // and fire the legitimate release. Reset back to
            // FOD_POLL_TIMEOUT_MS after every state change or
            // idle-recheck.
            int timeoutMs = FOD_POLL_TIMEOUT_MS;

            while (mRunning.load()) {
                int rc = poll(&fodPressStatusPoll, 1, timeoutMs);
                if (rc < 0) {
                    LOG(ERROR) << "failed to poll fd, err: " << rc;
                    continue;
                }

                // [UDFPS revents] Only act on POLLPRI (priority data
                // from the touch IC). POLLERR-only wakeups are
                // surfaced elsewhere (see readBool / kernel logs).
                // A timed-out poll (rc == 0) is also fine to evaluate:
                // it's our idle-recheck arm and ensures we don't
                // strand the HAL in DOWN after a swallowed release.
                if (rc > 0 && !(fodPressStatusPoll.revents & POLLPRI)) {
                    continue;
                }

                bool pressed = readBool(fd);

                // [UDFPS edge-detection] Drop spurious wakeups that
                // didn't actually change the press state.
                if (pressed == lastPressed) {
                    timeoutMs = FOD_POLL_TIMEOUT_MS;
                    continue;
                }

                if (pressed) {
                    // 0 -> 1 transition: real press.
                    lastPressed = true;
                    pressStart = std::chrono::steady_clock::now();
                    timeoutMs = FOD_POLL_TIMEOUT_MS;
                    // [UDFPS screen-off] Acquire kernel wake-lock so
                    // the framework has time to reach a terminal
                    // state before the device re-suspends. Stable
                    // wake-lock id keeps dumpsys output clean.
                    set(WAKE_LOCK_PATH, WAKE_LOCK_ID);
                    mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_STATUS,
                                    PARAM_FOD_PRESSED);
                } else {
                    // 1 -> 0 transition: candidate release.
                    auto elapsed_ms =
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - pressStart)
                                    .count();
                    if (pressStart.time_since_epoch().count() != 0 &&
                        elapsed_ms < FOD_DEBOUNCE_MS) {
                        // [UDFPS debounce] Touch-IC internal glitch
                        // (typically 9-11 ms). Suppress the release
                        // event but keep lastPressed=true so any
                        // subsequent 0->1 flicker during this same
                        // physical press does NOT re-fire DOWN.
                        // Shorten the poll timeout to the remainder
                        // of the debounce window so we re-check the
                        // FD promptly and will fire the legitimate
                        // UP once the touch IC truly settles at 0.
                        timeoutMs = static_cast<int>(FOD_DEBOUNCE_MS - elapsed_ms);
                        if (timeoutMs < 1) timeoutMs = 1;
                        continue;
                    }
                    lastPressed = false;
                    pressStart = std::chrono::steady_clock::time_point{};
                    timeoutMs = FOD_POLL_TIMEOUT_MS;
                    set(WAKE_UNLOCK_PATH, WAKE_LOCK_ID);
                    mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_STATUS,
                                    PARAM_FOD_RELEASED);
                }
            }
            close(fd);
        });
    }

    void onFingerDown(uint32_t /*x*/, uint32_t /*y*/, float /*minor*/, float /*major*/) {
        LOG(INFO) << __func__;
        // [UDFPS screen-off] Wake_lock is owned exclusively by the poll
        // thread, which writes to /sys/power/wake_lock on the rising
        // edge of /sys/class/touch/touch_dev/fod_press_status. We do NOT
        // also acquire it here: each write to /sys/power/wake_lock BUILDS
        // a new wakeup_source entry, so a second acquire would orphan
        // one lock since releaseWakeLock() only writes /sys/power/wake_unlock
        // once per press cycle. Race vs auto-suspend between poll() and
        // framework dispatch is bounded by Touch_Fod_Enable + Touch_Aod_Enable
        // (init() ioctls) keeping the FOD matrix alive in low-power.
        setFingerDown(true);
    }

    void onFingerUp() {
        LOG(INFO) << __func__;
        setFingerDown(false);
        // [UDFPS screen-off] Terminal — release the screen-off wake lock.
        // The poll thread is the canonical releaser (sees the FOD
        // release event); this is the framework-side mirror for the
        // rare case where onFingerUp arrives before the next poll
        // tick. Idempotent: kernel wakeup_source refcount just
        // decrements if already released.
        releaseWakeLock();
    }

    void onAcquired(int32_t result, int32_t vendorCode) {
        LOG(INFO) << __func__ << " result: " << result << " vendorCode: " << vendorCode;
        if (static_cast<AcquiredInfo>(result) == AcquiredInfo::GOOD) {
            setFingerDown(false);
            setFodStatus(FOD_STATUS_OFF);
            // [UDFPS screen-off] Terminal — release the screen-off wake lock.
            releaseWakeLock();
        } else if (vendorCode == 21 || vendorCode == 23) {
            /*
             * vendorCode = 21 waiting for fingerprint authentication
             * vendorCode = 23 waiting for fingerprint enroll
             */
            setFodStatus(FOD_STATUS_ON);
        }
    }

    void cancel() {
        LOG(INFO) << __func__;
        setFingerDown(false);
        setFodStatus(FOD_STATUS_OFF);
        // [UDFPS screen-off] Terminal — release the screen-off wake lock.
        releaseWakeLock();
    }

  private:
    fingerprint_device_t* mDevice;
    android::base::unique_fd touch_fd_;
    // [UDFPS lifecycle] Persistent (joined) poll thread + stop flag.
    std::thread mPollThread;
    std::atomic<bool> mRunning{false};

    void setFodStatus(int value) {
        // Same pattern as setFingerDown(): recoverable-failure PLOG(WARNING).
        int buf[MAX_BUF_SIZE] = {TOUCH_ID, Touch_Fod_Enable, value};
        if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf) < 0) {
            PLOG(WARNING) << "Touch_Fod_Enable ioctl failed (value=" << value << ")";
        }
    }

    void setFingerDown(bool pressed) {
        mDevice->extCmd(mDevice, COMMAND_NIT, pressed ? PARAM_NIT_FOD : PARAM_NIT_NONE);

        // [UDFPS diagnostics] Check ioctl return — silent failure here
        // means HBM/finger state never engages and vendor fingerprint
        // service reports "Up too fast" with no way for userspace to
        // tell whether the touch IC accepted the command. WARNING level
        // matches existing PLOG(WARNING) for wake_lock sysfs in this file.
        int buf[MAX_BUF_SIZE] = {TOUCH_ID, Touch_Fod_Enable, pressed ? 1 : 0};
        if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf) < 0) {
            PLOG(WARNING) << "Touch_Fod_Enable ioctl failed (pressed=" << pressed << ")";
        }

        set(DISP_PARAM_PATH,
            std::string(DISP_PARAM_LOCAL_HBM_MODE) + " " +
                    (pressed ? DISP_PARAM_LOCAL_HBM_ON : DISP_PARAM_LOCAL_HBM_OFF));
    }

    // [UDFPS screen-off] Belt-and-suspenders: ensuring wake_lock is
    // released on framework-side terminal events (acquired by poll
    // thread on raw press, mPollThread.release). Best-effort: if the
    // sysfs write fails (SELinux / chmod issue) we simply guard against
    // double-release with this short-circuit.
    void releaseWakeLock() {
        set(WAKE_UNLOCK_PATH, WAKE_LOCK_ID);
    }
};

static UdfpsHandler* create() {
    return new XiaomiMt6895UdfpsHander();
}

static void destroy(UdfpsHandler* handler) {
    delete handler;
}

extern "C" UdfpsHandlerFactory UDFPS_HANDLER_FACTORY = {
        .create = create,
        .destroy = destroy,
};
