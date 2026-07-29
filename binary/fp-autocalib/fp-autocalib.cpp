/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * fp-autocalib — CrDroid auto-recalibration helper for the Goodix
 * fingerprint sensor on Xiaomi MT6895 (plato).
 *
 * When the device's display panel is replaced (aftermarket repair), the
 * calibration data persisted in /mnt/vendor/persist/goodix/* no longer
 * matches the optical stack of the new panel, so the Goodix sensor
 * refuses to fingerprint reliably. Stock MIUI exposes a factory-mode
 * recalibration command that stock Engineering Mode (*#*#6484#*#*) uses.
 *
 * This helper invokes the same factory-mode recalibration entry point
 * of the vendor library directly, so the user does not have to open
 * Engineering Mode by hand. The entry point is reached through the
 * standard POSIX dlopen()/dlsym() ABI — no vendor code is copied,
 * decoded or modified.
 */

#include <android-base/logging.h>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <log/log.h>

#define LIB_PATH "/vendor/lib64/libgf_hal.so"

/* Itanium C++ ABI mangling of:
 *   int goodix::SZCustomizedProductTest::factoryCalibrate(unsigned int);
 * The mangled name is computed from public class+method signatures
 * already exposed in libgf_hal.so's .dynsym table.
 */
#define SYM_FACTORY_CALIBRATE \
    "_ZN6goodix23SZCustomizedProductTest16factoryCalibrateEj"

typedef int (*factory_calib_fn)(unsigned int group_id);

int main(int /*argc*/, char** /*argv*/) {
    ALOGI("fp-autocalib: starting");

    void* handle = dlopen(LIB_PATH, RTLD_NOW);
    if (handle == nullptr) {
        ALOGE("fp-autocalib: dlopen(%s) failed: %s", LIB_PATH, dlerror());
        return EXIT_FAILURE;
    }
    ALOGI("fp-autocalib: dlopen(%s) OK", LIB_PATH);

    (void)dlerror();

    auto* factory_calib =
        reinterpret_cast<factory_calib_fn>(dlsym(handle, SYM_FACTORY_CALIBRATE));
    const char* dlsym_err = dlerror();
    if (factory_calib == nullptr || dlsym_err != nullptr) {
        ALOGE("fp-autocalib: dlsym(%s) failed: %s",
              SYM_FACTORY_CALIBRATE,
              dlsym_err != nullptr ? dlsym_err : "null pointer");
        dlclose(handle);
        return EXIT_FAILURE;
    }
    ALOGI("fp-autocalib: dlsym(%s) OK", SYM_FACTORY_CALIBRATE);

    /* group_id = 0 selects the first calibration group (Berlin / L12A).
     * Returns 0 on success or one of the gf_error_t codes defined
     * inside libgf_hal.so. Anything non-zero is treated as failure. */
    int rc = factory_calib(0);
    ALOGI("fp-autocalib: factoryCalibrate(0) returned %d", rc);

    dlclose(handle);
    return (rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
