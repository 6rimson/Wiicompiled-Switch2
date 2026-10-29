// Desktop stand-ins for the HBLauncher runtime symbols that the Twilight Princess port's
// Dawn/SDL3/aurora patches call, so the console build (HBX_SWITCH2) can run headless on desktop
// aarch64 Linux. Linked instead of libhbxl.a; never part of the console package.
//   - no hbx_ctx: SDL's hbx audio driver declines and SDL falls back to its dummy driver;
//   - no Vulkan: Dawn's Vulkan backend fails and aurora falls back to Null, which "presents" to
//     the dummy vi window below without touching it;
//   - no input: the hbx joystick reports nothing pressed.
#include <stdlib.h>
#include <string.h>

#include "hbx_app.h"   /* HBLauncher sdk/include */

const struct hbx_ctx *hbxl_ctx;

typedef void (*hbxl_vk_void)(void);

hbxl_vk_void vk_icdGetInstanceProcAddr(void *instance, const char *name) {
    (void)instance;
    (void)name;
    return 0;
}

void *hbx_switch2_vi_window(void) {
    static char dummy_window[64];
    return dummy_window;
}

void hbx_input_poll(const struct hbx_ctx *ctx, struct hbx_input_state *state) {
    (void)ctx;
    memset(state, 0, sizeof *state);
}

void *hbxl_sdk_memalign(size_t align, size_t size) {
    void *p = 0;
    return posix_memalign(&p, align < sizeof(void *) ? sizeof(void *) : align, size) == 0 ? p : 0;
}

void hbxl_sdk_free(void *p) {
    free(p);
}
